import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_consumer_bc_filter import FORMATS, generate, reference, s3tc_rgb
from verify_bc_filter_witness import validate


class BCFilterReferenceTests(unittest.TestCase):
    def test_all_formats_have_a_deterministic_discriminating_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            shared = Path(directory) / 'reference.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-DBCDEC_IMPLEMENTATION',
                            '-DBCDEC_BC4BC5_PRECISE', '-x', 'c',
                            str(ROOT / 'third_party/bcdec/bcdec.h'), '-o', str(shared)], check=True)
            library = ctypes.CDLL(str(shared))
            for fmt in FORMATS:
                with self.subTest(format=fmt):
                    result = generate(library, fmt)
                    blocks, linear, nearest, distinct = result
                    self.assertEqual(len(blocks), 32 if fmt.startswith(('bc1_', 'bc4_')) else 64)
                    self.assertEqual(len(linear), 4096 * 4)
                    self.assertNotEqual(linear, nearest)
                    self.assertGreaterEqual(distinct, 1024)
                    self.assertEqual(result, generate(library, fmt))

    def test_s3tc_exact_normalized_endpoints_and_palette(self):
        color = bytes.fromhex('234f3c0d') + bytes.fromhex('e4e4e4e4')
        a, b = (9/31,57/63,3/31), (1/31,41/63,28/31)
        expected = [a, b, tuple((2*x+y)/3 for x,y in zip(a,b)),
                    tuple((x+2*y)/3 for x,y in zip(a,b))]*4
        self.assertEqual(s3tc_rgb(bytes(8)+color, 'bc3'), expected)
        self.assertEqual(s3tc_rgb(bytes(8)+color, 'bc2'), expected)
        self.assertEqual(s3tc_rgb(color, 'bc1'), expected)

    def test_s3tc_bc1_three_color_mode(self):
        color = bytes.fromhex('0000ffffe4e4e4e4')
        self.assertEqual(s3tc_rgb(color, 'bc1'),
                         [(0.,0.,0.), (1.,1.,1.), (.5,.5,.5), (0,0,0)]*4)

    def test_signed_filtering_precedes_destination_clamp(self):
        pixels = [[-1. if x < 4 else 1., 0., 0., 1.] for y in range(8) for x in range(8)]
        result = reference(pixels, True)
        self.assertEqual(result[31*4], 0)
        self.assertEqual(result[32*4], 32)


class BCFilterVerifierTests(unittest.TestCase):
    def setUp(self):
        self.contract = dict(format='VK_FORMAT_BC1_RGB_UNORM_BLOCK', format_value=131,
                             extent=[8,8], target_extent=[64,64], filter='linear', tolerance=2,
                             distinguishing_pixels=2048)
        for index, key in enumerate(('input_sha256','reference_sha256','nearest_sha256',
                                    'decoder_sha256','reference_generator_sha256','vert_spirv_sha256','frag_spirv_sha256')):
            self.contract[key] = str(index+1)*64
        self.artifact = dict(title='PPSA99994', profile='bc-linear-filter-witness', submit_enabled=True,
                             files={'eboot.bin':'a'*64}, bc_filter=self.contract)
        self.messages = [
            'PS5VK_CONSUMER_BC_FILTER_FEATURE textureCompressionBC=1 enabled_by_features2=1',
            'PS5VK_CONSUMER_BC_FILTER_START format=131 image=8x8 target=64x64 filter=linear '
            'input_sha256=' + self.contract['input_sha256'] + ' reference_sha256=' + self.contract['reference_sha256'],
            'PS5VK_CONSUMER_BC_FILTER_RESULT pixels=4096 mismatches=0 max_error=1 tolerance=2',
            'PS5VK_CONSUMER_BC_FILTER_RETIRED fence_complete=1 allocations=0',
            'PS5VK_CONSUMER_TEST_SUCCESS',
            'PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1',
            'PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1']

    def run_validation(self):
        lines = ['HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=123']
        lines += [f'{i}\t{i}\tMARK\t{message}' for i,message in enumerate(self.messages,1)]
        lines += [f'BYE seq={len(self.messages)} reason=consumer-bc-filter-end']
        log = ('\n'.join(lines)+'\n').encode()
        receipt = dict(protocol='ps5log/1',transport='tcp',clean=True,bye=True,gaps=[],raw_lines=0,
                       identity=dict(title='PPSA99994',app='ps5vk',boot='123'),
                       last_seq=len(self.messages),sha256=hashlib.sha256(log).hexdigest())
        return validate(log, receipt, self.artifact)

    def test_exact_run(self):
        self.assertEqual(self.run_validation()['pixels_checked'],4096)

    def test_wrong_reference_refused(self):
        self.contract['reference_sha256']='f'*64
        with self.assertRaisesRegex(ValueError,'reference identity'):
            self.run_validation()

    def test_nearest_equivalent_reference_refused(self):
        self.contract['distinguishing_pixels']=0
        with self.assertRaisesRegex(ValueError,'reject nearest'):
            self.run_validation()

    def test_bad_gpu_output_refused(self):
        self.messages[2]=self.messages[2].replace('mismatches=0','mismatches=1')
        with self.assertRaisesRegex(ValueError,'GPU filtered pixels'):
            self.run_validation()

    def test_retirement_required(self):
        self.messages.remove('PS5VK_CONSUMER_BC_FILTER_RETIRED fence_complete=1 allocations=0')
        with self.assertRaises(ValueError):
            self.run_validation()


if __name__ == '__main__':
    unittest.main()
