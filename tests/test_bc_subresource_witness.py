import hashlib
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_consumer_bc_subresource import generate, PROFILES
from verify_bc_subresource_witness import validate, PREFIX


class BCSubresourceReferenceTests(unittest.TestCase):
    def test_profile_shapes_and_exact_preservation(self):
        for profile, (_, _, mip, block_bytes) in PROFILES.items():
            with self.subTest(profile=profile):
                upload, raw, pixels, regions, patch, contract = generate(profile)
                self.assertEqual(generate(profile), generate(profile))
                self.assertEqual(len(regions), 12)
                self.assertEqual(len(pixels), 64*64*4)
                self.assertEqual(contract['selected_extent'], [max(1,n>>mip) for n in contract['extent']])
                if profile == 'bc1-partial-layers':
                    continue  # The independent block-by-block oracle below covers this profile.
                layer = contract['selected_layer']
                touched = set()
                for r in regions:
                    width = ((r['width']+3)//4)*block_bytes
                    for y in range(r['rows']):
                        at = r['offset']+y*r['pitch']
                        source = patch+y*r['pitch'] if r['layer']==layer and r['mip']==mip else at
                        self.assertEqual(raw[at:at+width], upload[source:source+width])
                        if source != at:
                            self.assertNotEqual(raw[at:at+width], upload[at:at+width])
                        touched.update(range(at,at+width))
                self.assertTrue(all(v==0xa5 for i,v in enumerate(raw) if i not in touched))
                self.assertGreater(len(raw)-len(touched), 12*32)
                # Wrong mip, layer, or stale pre-overwrite texels must be observable.
                selected = regions[layer*4+mip]
                self.assertNotEqual(upload[patch:patch+block_bytes], upload[selected['offset']:selected['offset']+block_bytes])
                self.assertTrue(all(upload[r['offset']:r['offset']+block_bytes] != upload[patch:patch+block_bytes] for r in regions
                                    if profile != 'bc1-imagecopy' or r['layer'] != 0 or r['mip'] != mip))

    def test_known_bc1_and_bc3_samples(self):
        for profile in PROFILES:
            upload, _, pixels, regions, patch, contract = generate(profile)
            if profile == 'bc1-partial-layers':
                patch = regions[contract['selected_layer']*4+contract['selected_mip']]['offset']
            block_bytes = PROFILES[profile][3]
            at = patch+(8 if block_bytes==16 else 0)
            endpoint = int.from_bytes(upload[at:at+2], 'little')
            expected = [int(((endpoint>>11)&31)*255/31+.5),
                        int(((endpoint>>5)&63)*255/63+.5),
                        int((endpoint&31)*255/31+.5),
                        upload[patch] if block_bytes==16 else 255]
            self.assertEqual(list(pixels[:4]), expected)


    def test_literal_selected_pixel_oracles(self):
        # Fixed RGB565 endpoints (13,31,9) and (20,42,22), independently
        # normalized and rounded. A 6-texel nearest image changes block at
        # output x=43; its one-texel BC3 tail has alpha endpoint 141.
        bc1 = generate('bc1-mip')[2]
        first, second = bytes((107,125,74,255)), bytes((165,170,181,255))
        self.assertEqual(bc1, (first*43 + second*21)*64)
        self.assertEqual(generate('bc3-tail')[2], bytes((107,125,74,141))*4096)
        first, second = bytes((247,16,115,255)), bytes((49,61,222,255))
        self.assertEqual(generate('bc1-imagecopy')[2], (first*43 + second*21)*64)

    def test_odd_extent_layer_pixel_oracle(self):
        # Independent nearest boundaries for 13x9: x=20,39,59; y=28,57.
        # The one-texel right/bottom edges must survive both upload and sample.
        colors = ((107,125,74,255), (165,170,181,255), (222,215,33,255), (25,4,140,255),
                  (82,49,247,255), (140,93,99,255), (197,138,206,255), (255,182,58,255),
                  (58,227,165,255), (115,16,16,255), (173,61,123,255), (230,105,230,255))
        expected = bytearray()
        for row, height in enumerate((28,29,7)):
            scanline = b''.join(bytes(colors[row*4+column])*width
                                for column,width in enumerate((20,19,20,5)))
            expected.extend(scanline*height)
        self.assertEqual(generate('bc1-layer')[2], expected)

    def test_wrong_mip_layer_and_stale_texels_are_rejected(self):
        def candidate(upload, region, block_bytes):
            out = bytearray()
            for y in range(64):
                for x in range(64):
                    sx = int((x+.5)*region['width']/64)
                    sy = int((y+.5)*region['height']/64)
                    at = region['offset']+(sy//4)*region['pitch']+(sx//4)*block_bytes
                    color = at+(8 if block_bytes==16 else 0)
                    endpoint = int.from_bytes(upload[color:color+2], 'little')
                    out.extend((int(((endpoint>>11)&31)*255/31+.5),
                                int(((endpoint>>5)&63)*255/63+.5),
                                int((endpoint&31)*255/31+.5),
                                upload[at] if block_bytes==16 else 255))
            return out
        for profile in PROFILES:
            upload, raw, expected, regions, patch, contract = generate(profile)
            mip, layer = contract['selected_mip'], contract['selected_layer']
            block_bytes=PROFILES[profile][3]
            candidates = [('stale-upload',layer*4+mip)]
            # Include the common failure that ignores baseMipLevel and samples
            # mip zero, as well as every other stored mip.
            candidates += [(f'wrong-mip-{other}',layer*4+other) for other in range(4)
                           if other != mip]
            candidates += [(f'wrong-layer-{other}',other*4+mip) for other in range(3)
                           if other != layer and not (profile == 'bc1-imagecopy' and other == 0)]
            for label, index in candidates:
                with self.subTest(profile=profile, fault=label):
                    actual = candidate(upload,regions[index],block_bytes)
                    mismatches = sum(any(abs(a-b)>1 for a,b in zip(actual[i:i+4],expected[i:i+4]))
                                     for i in range(0,len(expected),4))
                    self.assertGreaterEqual(mismatches, 500 if profile == 'bc1-partial-layers' and label == 'stale-upload' else 1024)
            if profile == 'bc1-partial-layers':
                continue  # Full raw and stride faults are checked separately below.
            # Copying the replacement into layer zero instead of the selected layer
            # corrupts a preserved subresource and leaves the selected one stale.
            wrong = bytearray(raw)
            selected, other = regions[layer*4+mip], regions[mip]
            row_bytes=((selected['width']+3)//4)*block_bytes
            for y in range(selected['rows']):
                at=selected['offset']+y*selected['pitch']
                to=other['offset']+y*other['pitch']
                source=patch+y*selected['pitch']
                wrong[at:at+row_bytes]=upload[at:at+row_bytes]
                wrong[to:to+row_bytes]=upload[source:source+row_bytes]
            self.assertNotEqual(wrong,raw)

    def test_row_padding_and_inter_region_stomps_are_rejected(self):
        for profile in PROFILES:
            _, raw, _, regions, _, _ = generate(profile)
            block_bytes=PROFILES[profile][3]
            r=regions[0]
            # All transferred blocks can remain correct while a writer
            # corrupts just one unselected byte. Both guard classes count.
            padding=r['offset']+((r['width']+3)//4)*block_bytes
            gap=r['offset']+r['rows']*r['pitch']
            for offset in (0, padding, gap, len(raw)-1):
                with self.subTest(profile=profile, offset=offset):
                    self.assertEqual(raw[offset],0xa5)
                    wrong=bytearray(raw); wrong[offset]=0xcd
                    self.assertEqual(sum(a!=b for a,b in zip(wrong,raw)),1)

    def test_partial_multilayer_blocks_strides_and_exterior_preservation(self):
        upload, raw, pixels, regions, patch, contract = generate('bc1-partial-layers')
        self.assertEqual(contract['selected_extent'], [18,14])
        self.assertEqual(contract['preserved_subresources'], 10)
        touched=set(); changed=[]
        for r in regions:
            for y in range(r['rows']):
                for x in range((r['width']+3)//4):
                    at=r['offset']+y*r['pitch']+x*8
                    is_patch=r['mip']==1 and r['layer'] in (1,2) and y==1 and x in (1,2)
                    source=patch+(r['layer']-1)*48+(x-1)*8 if is_patch else at
                    self.assertEqual(raw[at:at+8],upload[source:source+8])
                    if is_patch:
                        changed.append((r['layer'],x,y))
                        self.assertNotEqual(raw[at:at+8],upload[at:at+8])
                    touched.update(range(at,at+8))
        self.assertEqual(changed,[(1,1,1),(1,2,1),(2,1,1),(2,2,1)])
        read=contract['partial_read_offset']
        for layer in range(2):
            self.assertEqual(raw[read+layer*96:read+layer*96+16],
                             upload[patch+layer*48:patch+layer*48+16])
            touched.update(range(read+layer*96,read+layer*96+16))
        self.assertTrue(all(v==0xa5 for i,v in enumerate(raw) if i not in touched))
        self.assertGreater(len(raw)-len(touched), 900)
        # Collapsing the upload/readback layer stride, or swapping layers,
        # cannot reproduce the expected interior bytes.
        self.assertNotEqual(upload[patch:patch+16],upload[patch+48:patch+64])
        self.assertNotEqual(upload[patch+24:patch+40],upload[patch+48:patch+64])
        self.assertEqual(raw[read+32:read+48],bytes([0xa5])*16)
        self.assertEqual(raw[read+64:read+80],bytes([0xa5])*16)
        # Independent normalized endpoints over the complete 18x14 image.
        # Integer nearest mapping avoids the generator's floating point path.
        expected=bytearray()
        for y in range(64):
            for x in range(64):
                bx=((2*x+1)*18//128)//4; by=((2*y+1)*14//128)//4
                seed=701+bx-1 if by==1 and bx in (1,2) else 53+17+by*5+bx
                rgb=(((seed*7+3)%31+1,31),((seed*11+5)%63+1,63),((seed*13+9)%31+1,31))
                expected.extend((v*255+den//2)//den for v,den in rgb)
                expected.append(255)
        self.assertEqual(pixels,expected)


class BCConsumerPhysicalQueryTests(unittest.TestCase):
    def test_shipping_and_both_bc_profiles_match_public_queries(self):
        # Reuse the actual public query test platform and the unchanged
        # consumer assertions. Discard unrelated test entry points at link.
        source = '#define main unused_device_test_main\n#include "tests/test_vk_device.c"\n#undef main\nint main(void) { consumer_physical_queries(); return 0; }\n'
        with tempfile.TemporaryDirectory() as directory:
            main = Path(directory)/'consumer_queries.c'
            main.write_text(source)
            for name, flags in (
                ('shipping', []),
                ('filter', ['-DCONSUMER_BC_FILTER_WITNESS=1']),
                ('subresource', ['-DCONSUMER_BC_SUBRESOURCE_WITNESS=1'])):
                with self.subTest(profile=name):
                    binary=Path(directory)/name
                    compiled = subprocess.run(['cc','-std=c11','-ffunction-sections','-fdata-sections',
                                    '-I'+str(ROOT), '-I'+str(ROOT/'src'),
                                    '-I'+str(ROOT/'third_party/vulkan-headers/include'),
                                    *flags, str(main),
                                    *[str(ROOT/'src'/n) for n in ('vk_device.c','vk_alloc.c','vk_memory.c','texture_format.c','texture_layout.c')],
                                    '-Wl,--gc-sections','-o',str(binary)],check=False,
                                   capture_output=True,text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(executed.returncode, 0, executed.stderr)


class BCSubresourceVerifierTests(unittest.TestCase):
    def setUp(self):
        self.contract = generate('bc1-mip')[-1]
        self.contract.update(vert_spirv_sha256='1'*64, frag_spirv_sha256='2'*64)
        c = self.contract
        self.artifact = dict(title='PPSA99994',profile='bc-subresource-witness',submit_enabled=True,
                             files={'eboot.bin':'a'*64},bc_subresource=c)
        self.messages = [PREFIX+'FEATURE textureCompressionBC=1 enabled_by_features2=1',
            PREFIX+f"START profile=bc1-mip format=131 image=13x9 mips=4 layers=3 mip=1 layer=2 input_sha256={c['input_sha256']} raw_reference_sha256={c['raw_reference_sha256']} reference_sha256={c['reference_sha256']}",
            PREFIX+'FENCE complete=1 timeout_ns=300000000',
            PREFIX+f"RAW bytes={c['readback_bytes']} subresources=12 preserved=11 mismatches=0",
            PREFIX+'RESULT pixels=4096 mismatches=0 max_error=1 tolerance=1',
            PREFIX+'RETIRED fence_complete=1 allocations=0',
            'PS5VK_CONSUMER_TEST_SUCCESS',
            'PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1',
            'PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1']

    def evidence(self):
        lines = ['HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=123']
        lines += [f'{i}\t{i}\tMARK\t{m}' for i,m in enumerate(self.messages,1)]
        lines += [f'BYE seq={len(self.messages)} reason=consumer-bc-subresource-end']
        log = ('\n'.join(lines)+'\n').encode()
        receipt = dict(protocol='ps5log/1',transport='tcp',clean=True,bye=True,gaps=[],raw_lines=0,
                       identity=dict(title='PPSA99994',app='ps5vk',boot='123'),
                       last_seq=len(self.messages),sha256=hashlib.sha256(log).hexdigest())
        return log, receipt

    def run_validation(self):
        return validate(*self.evidence(), self.artifact)

    def test_exact_run(self):
        self.assertEqual(self.run_validation()['preserved_subresources'],11)

    def switch_profile(self, profile):
        old = self.contract
        self.contract = generate(profile)[-1]
        self.contract.update(vert_spirv_sha256='1'*64, frag_spirv_sha256='2'*64)
        self.artifact['bc_subresource'] = self.contract
        for key in ('profile', 'format_value', 'selected_mip', 'selected_layer', 'input_sha256',
                    'raw_reference_sha256', 'reference_sha256', 'readback_bytes', 'preserved_subresources'):
            # Replace named fields, avoiding unrelated occurrences of 1 or 131.
            field = {'format_value':'format', 'selected_mip':'mip', 'selected_layer':'layer',
                     'readback_bytes':'bytes', 'preserved_subresources':'preserved'}.get(key, key)
            self.messages = [m.replace(f"{field}={old[key]}", f"{field}={self.contract[key]}")
                             for m in self.messages]

        self.messages = [m.replace('image='+'x'.join(map(str,old['extent'])),
                                   'image='+'x'.join(map(str,self.contract['extent'])))
                         for m in self.messages]

    def test_partial_layers_run_and_geometry_identity(self):
        self.switch_profile('bc1-partial-layers')
        result = self.run_validation()
        self.assertEqual(result['preserved_subresources'], 10)
        for key,value in (('copy_layers',1), ('copy_offset',[0,0,0]),
                          ('copy_extent',[7,4,1]), ('upload_image_height',4),
                          ('readback_image_height',8), ('extent',[13,9])):
            old=self.contract[key]; self.contract[key]=value
            with self.assertRaisesRegex(ValueError,'independent contract'):
                self.run_validation()
            self.contract[key]=old

    def test_bc3_tail_run(self):
        self.switch_profile('bc3-tail')
        self.assertEqual(self.run_validation()['selected_mip'], 3)

    def test_base_mip_layer_run(self):
        self.switch_profile('bc1-layer')
        result = self.run_validation()
        self.assertEqual((result['selected_mip'], result['selected_layer']), (0, 1))
        self.assertEqual(result['preserved_subresources'], 11)
        self.assertEqual(result['operation'], 'buffer-to-image')

    def test_runtime_mip_and_layer_identity_cannot_drift(self):
        for profile in PROFILES:
            self.setUp()
            self.switch_profile(profile)
            start = self.messages[1]
            for field, original, wrong in (
                ('mip', self.contract['selected_mip'], (self.contract['selected_mip']+1)%4),
                ('layer', self.contract['selected_layer'], 0)):
                with self.subTest(profile=profile, field=field):
                    self.messages[1] = start.replace(f'{field}={original} ', f'{field}={wrong} ')
                    with self.assertRaisesRegex(ValueError, 'input and reference identity'):
                        self.run_validation()
            self.messages[1] = start

    def test_image_copy_run_and_source_identity(self):
        self.switch_profile('bc1-imagecopy')
        result = self.run_validation()
        self.assertEqual(result['operation'], 'image-to-image')
        self.assertEqual(result['source_layer'], 0)
        self.assertEqual(result['selected_layer'], 2)
        self.contract['source_layer'] = 1
        with self.assertRaisesRegex(ValueError, 'independent contract'):
            self.run_validation()

    def test_every_required_marker_is_mandatory_unique_and_ordered(self):
        original = list(self.messages)
        for i in range(len(original)):
            for action in ('remove','duplicate','reorder'):
                with self.subTest(index=i,action=action):
                    self.messages = list(original)
                    if action=='remove': self.messages.pop(i)
                    elif action=='duplicate': self.messages.append(self.messages[i])
                    else:
                        j=(i+1)%len(original)
                        self.messages[i],self.messages[j]=self.messages[j],self.messages[i]
                    with self.assertRaises(ValueError): self.run_validation()
        self.messages=original

    def test_corrupt_raw_pixel_fence_and_retirement_results(self):
        for index, before, after in ((2,'complete=1','complete=0'),(3,'mismatches=0','mismatches=1'),
                                    (3,'preserved=11','preserved=10'),(4,'max_error=1','max_error=2'),
                                    (5,'allocations=0','allocations=1')):
            original=self.messages[index]
            self.messages[index]=original.replace(before,after)
            with self.assertRaises(ValueError): self.run_validation()
            self.messages[index]=original

    def test_mismatch_diagnostic_cannot_coexist_with_success(self):
        self.messages.insert(3, PREFIX+'BYTE index=32 actual=0 expected=1')
        with self.assertRaisesRegex(ValueError, 'contradictory'):
            self.run_validation()

    def test_artifact_oracle_cannot_be_forged(self):
        for key,value in (('raw_reference_sha256','f'*64),('selected_mip',0),('readback_bytes',1),('tolerance',255)):
            original=self.contract[key]; self.contract[key]=value
            with self.assertRaises(ValueError): self.run_validation()
            self.contract[key]=original

    def test_partial_wrong_identity_or_bad_hash_receipt(self):
        for key,value in (('clean',False),('bye',False),('gaps',[1]),('raw_lines',1),('sha256','f'*64),('last_seq',100),('identity',dict(title='wrong',app='ps5vk',boot='123'))):
            log, receipt=self.evidence(); receipt[key]=value
            with self.assertRaises(ValueError): validate(log,receipt,self.artifact)

    def test_wrong_bye_sequence(self):
        log,receipt=self.evidence(); log=log.replace(b'BYE seq=9',b'BYE seq=8')
        receipt['sha256']=hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError,'BYE sequence'): validate(log,receipt,self.artifact)


if __name__ == '__main__':
    unittest.main()
