import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('camera_preview_device', Path(__file__).with_name('camera_preview_device.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CurrentCameraLaunch(unittest.TestCase):
    def test_old_success_cannot_make_a_new_failed_launch_pass(self):
        text = ('Queueing launch request: publisher:pxa-camera\nGC2145 ready:320x240\n'
                'Camera preview: frames=100 elapsed_ms=5000\n'
                'Queueing launch request: publisher:pxa-camera\nCamera failed to initialize\n')
        with self.assertRaisesRegex(ValueError, 'did not initialize'):
            module.current_launch_samples(text, 'publisher:pxa-camera')

    def test_only_current_identity_and_launch_are_measured(self):
        text = ('Queueing launch request: publisher:pxa-camera\nGC2145 ready:320x240\n'
                'Camera preview: frames=100 elapsed_ms=5000\n'
                'Queueing launch request: publisher:pxa-camera\nGC2145 ready:320x240\n'
                'RGB preview frame=61 acquire_us=20 convert_us=40 alloc_us=5 pixels_us=30 return_us=5\n'
                'Camera preview: frames=65 elapsed_ms=5000\n')
        samples, stages = module.current_launch_samples(text, 'publisher:pxa-camera')
        self.assertEqual([s['fps'] for s in samples], [13])
        self.assertEqual(stages[0]['pixels_us'], 30)
        with self.assertRaisesRegex(ValueError, 'launch marker'):
            module.current_launch_samples(text, 'another:pxa-camera')

    def test_invalid_duration_is_not_a_frame_rate(self):
        with self.assertRaisesRegex(ValueError, 'duration'):
            module.current_launch_samples('Queueing launch request: p\nGC2145 ready:x\nCamera preview: frames=65 elapsed_ms=0', 'p')


if __name__ == '__main__':
    unittest.main()
