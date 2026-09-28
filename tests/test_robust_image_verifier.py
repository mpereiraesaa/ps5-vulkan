# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic logs validate the verifier; never native execution evidence."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.verify_robust_image_witness import CASES, fixture_contract, digest, verify
from tools.robust_image_witness import coordinates, expected_read
from tools.build_upstream_cts import tessellation_build_profile

ROOT = Path(__file__).resolve().parents[1]


def fixture(name, alpha=0):
    case=CASES[name];contract=fixture_contract(name)
    markers=[f'ROBUST_IMAGE_START contract=1 case={name} count={contract["count"]} write={int(case.write)}']
    words=[]
    for index,coordinate in enumerate(coordinates(case)):
        allowed=((index,0x7351,0,1),) if case.write else expected_read(case,coordinate)
        values=allowed[alpha if len(allowed)>1 else 0];words.extend(values)
        markers.append(f'ROBUST_IMAGE_SAMPLE index={index} value='+','.join(f'{v:08x}' for v in values))
    resource=contract['resource']
    markers.append(f'ROBUST_IMAGE_RESOURCE mode={resource["mode"]} bytes={resource["bytes"]} digest={resource["digest"]}')
    markers.append(f'ROBUST_IMAGE_RESULT outputs={contract["count"]} mismatches=0 image_changes=0 input_changes=0 guards=0 digest={digest(words)} fence=complete')
    markers.append('ROBUST_IMAGE_RETIRED resources=clean')
    log=('\n'.join('[MARK] '+m for m in markers)+'\n').encode()
    receipt={'protocol':'ps5log/1','title':'PPSA99994','app':'ps5vk','transport':'tcp',
        'clean':True,'bye':True,'gaps':0,'sha256':hashlib.sha256(log).hexdigest(),'run_id':'synthetic-control'}
    eboot=b'synthetic local executable fixture, not a native payload'
    artifact={**contract,**{k:'a'*64 for k in ('sdk_sha256','source_sha256','helper_sha256','header_sha256','shader_sha256')},
        'eboot_sha256':hashlib.sha256(eboot).hexdigest(),
        'build_profile':tessellation_build_profile({'PS5VK_IMAGE_ROBUSTNESS_DIAGNOSTIC':'1'})}
    return log,receipt,artifact,eboot


class ImageWitnessVerifier(unittest.TestCase):
    def rejected_log(self,args,log):
        receipt=dict(args[1],sha256=hashlib.sha256(log).hexdigest())
        with self.assertRaises(ValueError):verify(log,receipt,args[2],args[3])

    def test_all_cases_and_both_permitted_alphas(self):
        for name in CASES:
            for alpha in (0,1):
                with self.subTest(case=name,alpha=alpha):
                    result=verify(*fixture(name,alpha))
                    self.assertTrue(result['strict_verified'])
                    self.assertTrue(result['local_artifact_identity_verified'])
                    self.assertFalse(result['deployment_identity_verified'])
                    self.assertFalse(result['firmware_verified'])
                    self.assertFalse(result['native_feature_conformance_verified'])

    def test_every_missing_duplicate_reordered_and_corrupt_sample(self):
        for name in CASES:
            args=fixture(name);lines=args[0].splitlines(keepends=True)
            for i in range(len(lines)):
                self.rejected_log(args,b''.join(lines[:i]+lines[i+1:]))
                self.rejected_log(args,b''.join(lines[:i]+[lines[i]]+lines[i:]))
            for i in range(1,len(coordinates(CASES[name]))+1):
                changed=list(lines);changed[i]=changed[i].split(b' value=')[0]+b' value=ffffffff,ffffffff,ffffffff,ffffffff\n'
                self.rejected_log(args,b''.join(changed))
            changed=list(lines);changed[1],changed[2]=changed[2],changed[1]
            self.rejected_log(args,b''.join(changed))
            for bad in (b'ROBUST_IMAGE_FAILURE result=-13',b'ROBUST_IMAGE_PENDING resources=retained'):
                self.rejected_log(args,args[0]+bad+b'\n')

    def test_flags_digests_oracles_and_wrong_case(self):
        args=fixture('storage-2d-r32-write')
        for old,new in ((b'mismatches=0',b'mismatches=1'),(b'image_changes=0',b'image_changes=1'),
                (b'input_changes=0',b'input_changes=1'),(b'guards=0',b'guards=1'),
                (b'fence=complete',b'fence=pending'),(b'resources=clean',b'resources=retained'),
                (b'mode=image-copy',b'mode=fetch-only'),(b'contract=1',b'contract=0'),
                (b'write=1',b'write=0'),(b'digest=',b'digest=0'),
                (b'storage-2d-r32-write',b'storage-texel-r32-write')):
            self.rejected_log(args,args[0].replace(old,new))
        self.rejected_log(args,args[0]+b'\xff')
        # Robust buffer access2 requires missing alpha1, unlike image robustness1.
        args=fixture('uniform-texel-r32')
        lines=args[0].splitlines(keepends=True)
        index=next(i for i,c in enumerate(coordinates(CASES['uniform-texel-r32'])) if c[0]<0)
        lines[index+1]=lines[index+1].replace(b'00000000,00000000,00000000,00000001',b'00000000,00000000,00000000,00000000')
        self.rejected_log(args,b''.join(lines))

    def test_false_numerical_result_with_recomputed_digests_is_rejected(self):
        args=fixture('sampled-2d-r32')
        lines=args[0].decode().splitlines()
        payload=lines[1].split(' value=')[1].split(',')
        payload[0]=f'{int(payload[0],16)^1:08x}'
        lines[1]=lines[1].split(' value=')[0]+' value='+','.join(payload)
        values=[]
        for line in lines:
            if 'ROBUST_IMAGE_SAMPLE ' in line:
                values.extend(int(v,16) for v in line.split(' value=')[1].split(','))
        lines[-2]=lines[-2].split('digest=')[0]+'digest='+digest(values)+' fence=complete'
        forged=('\n'.join(lines)+'\n').encode()
        receipt=dict(args[1],sha256=hashlib.sha256(forged).hexdigest())
        with self.assertRaisesRegex(ValueError,'numerical oracle'):
            verify(forged,receipt,args[2],args[3])

    def test_receipt_contract_and_binary_identity(self):
        args=fixture('sampled-2d-rgba8')
        for key,bad in {'protocol':'udp','title':'other','app':'other','transport':'udp',
                'clean':False,'bye':False,'gaps':1,'sha256':'0'*64,'run_id':' '}.items():
            receipt=dict(args[1]);receipt[key]=bad
            with self.subTest(key=key), self.assertRaises(ValueError):verify(args[0],receipt,args[2],args[3])
        for key in args[1]:
            receipt=dict(args[1]);del receipt[key]
            with self.assertRaises(ValueError):verify(args[0],receipt,args[2],args[3])
        for key in args[2]:
            artifact=copy.deepcopy(args[2]);del artifact[key]
            with self.assertRaises(ValueError):verify(args[0],args[1],artifact,args[3])
        for key,bad in {'contract_version':True,'count':True,'write':0,'fixture_sha256':{},'resource':{},
                'build_profile':{},'eboot_sha256':'f'*64,'case':'unknown','coordinates':[]}.items():
            artifact=copy.deepcopy(args[2]);artifact[key]=bad
            with self.subTest(key=key), self.assertRaises(ValueError):verify(args[0],args[1],artifact,args[3])
        for key in ('PS5VK_IMAGE_ROBUSTNESS_DIAGNOSTIC','PS5VK_INTEGER_DOT_DIAGNOSTIC'):
            artifact=copy.deepcopy(args[2]);artifact['build_profile']['switches'][key]='1' if key.endswith('INTEGER_DOT_DIAGNOSTIC') else '0'
            with self.assertRaises(ValueError):verify(args[0],args[1],artifact,args[3])
        with self.assertRaises(ValueError):verify(args[0],args[1],args[2],args[3]+b'changed')
        receipt=dict(args[1],gaps=False)
        with self.assertRaises(ValueError):verify(args[0],receipt,args[2],args[3])

    def test_offline_cli_overwrites_stale_success_on_failure(self):
        log,receipt,artifact,eboot=fixture('storage-texel-r32-write')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name,value in {'log':log,'receipt':json.dumps(receipt).encode(),
                    'artifact':json.dumps(artifact).encode(),'eboot':eboot}.items():(root/name).write_bytes(value)
            command=['python3',str(ROOT/'tools/verify_robust_image_witness.py')]
            for name in ('log','receipt','artifact','eboot','out'):command+=['--'+name,str(root/name)]
            result=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(0,result.returncode,result.stderr)
            self.assertTrue(json.loads((root/'out').read_text())['strict_verified'])
            (root/'eboot').write_bytes(b'wrong executable')
            result=subprocess.run(command,capture_output=True,text=True)
            self.assertNotEqual(0,result.returncode)
            self.assertFalse(json.loads((root/'out').read_text())['strict_verified'])
