import unittest
from tools.check_continuous_ownership import audit


def frame(n):
    phase,slot=n%180,n%2
    return [f"PS5VK_GRAPHICS_REUSE_BEGIN frame={phase} slot={slot} offset={slot*0x4000000}",
            f"PS5VK_GRAPHICS_PREPARED serial={n+7} draws=1",
            f"PS5VK_GRAPHICS_SUBMIT serial={n+7} rc=0",
            f"PS5VK_GRAPHICS_SUSPEND_POINT serial={n+7} rc=0",
            f"PS5VK_GRAPHICS_COMPLETED serial={n+7}",
            "PS5VK_GRAPHICS_API_READBACK valid=1 bad_alpha=0 bad_sum=0",
            f"PS5VK_TEXTURE_READBACK frame={phase} unexpected=0",
            f"PS5VK_VIDEO_SUBMIT token={n+1} rc=0",
            f"PS5VK_VIDEO_SUSPEND_POINT token={n+1} rc=0",
            f"PS5VK_VIDEO_PRESENTED token={n+1} fence=0 matching_event=1 hold_seconds=0",
            f"PS5VK_GRAPHICS_REUSE_END frame={phase} slot={slot} displayed={slot}"]


def log(messages):
    messages=["PS5VK_SCENE_LOOP mode=continuous pause_us=0"]+messages
    return "HELLO\n"+"".join(f"{i}\t{i}\tMARK\t{m}\n" for i,m in enumerate(messages,1))


class ContinuousOwnershipTests(unittest.TestCase):
    def heartbeat_log(self, second_ns=2020000000):
        records=[]
        for n in range(121):
            records.extend(frame(n)[:-1])
            if n in (0,120):
                elapsed=20000000 if n==0 else second_ns
                records.append(f"PS5VK_SCENE_HEARTBEAT frames={n+1} elapsed_ns={elapsed} token={n+1}")
            records.append(frame(n)[-1])
        return log(records)

    def test_cadence_measures_intervals_not_startup(self):
        cadence=audit(self.heartbeat_log())["presentation_loop_cadence"]
        self.assertEqual(cadence["mean_fps"],60)
        self.assertEqual(cadence["interval_count"],1)
        self.assertEqual(audit(self.heartbeat_log(4020000000))["presentation_loop_cadence"]["mean_fps"],30)
        self.assertIsNone(audit(log(frame(0)))["presentation_loop_cadence"]["mean_fps"])

    def test_bad_heartbeat_rejected(self):
        valid=self.heartbeat_log()
        for old,new in [("frames=121","frames=120"),("token=121","token=120"),
                        ("elapsed_ns=2020000000","elapsed_ns=20000000")]:
            with self.subTest(new=new),self.assertRaises(ValueError):
                audit(valid.replace(old,new))

    def test_wrap_uses_full_tokens_and_alternating_slots(self):
        self.assertEqual(audit(log(sum((frame(i) for i in range(182)),[])))["completed_frames"],182)

    def test_every_missing_middle_record_rejected(self):
        for index in range(11):
            records=frame(0)+frame(1)
            records.pop(index)
            with self.subTest(index=index),self.assertRaises(ValueError):audit(log(records))

    def test_final_prefix_is_not_counted_as_completed(self):
        for length in range(1,11):
            result=audit(log(frame(0)+frame(1)[:length]))
            self.assertEqual(result["completed_frames"],1)
            self.assertIsNotNone(result["incomplete_tail_stage"])
            self.assertFalse(result["os_close_proven"])

    def test_corrupted_state_rejected(self):
        for index,old,new in [(0,"slot=0","slot=1"),(0,"offset=0","offset=16"),
                (2,"rc=0","rc=-1"),(3,"serial=7","serial=8"),
                (5,"valid=1","valid=0"),(6,"unexpected=0","unexpected=1"),
                (7,"token=1","token=2"),(8,"rc=0","rc=-1"),
                (9,"matching_event=1","matching_event=0"),(10,"displayed=0","displayed=1")]:
            records=frame(0)
            records[index]=records[index].replace(old,new)
            with self.subTest(index=index),self.assertRaises(ValueError):audit(log(records))
