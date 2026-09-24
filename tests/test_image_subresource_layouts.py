from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ImageSubresourceLayouts(unittest.TestCase):
    def test_public_barrier_order_and_atomic_ranges(self):
        commands = subprocess.check_output(["make", "-n", "check"], cwd=ROOT, text=True)
        command = next(line for line in commands.splitlines()
                       if "tests/test_image_copy_clear.c" in line and " -o " in line)
        args = shlex.split(command)
        args[args.index("tests/test_image_copy_clear.c")] = "tests/test_image_subresource_layouts.c"
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "layouts")
            args[args.index("-o") + 1] = binary
            subprocess.run(args, cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True)

if __name__ == "__main__":
    unittest.main()
