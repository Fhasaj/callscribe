import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.db.database import connect
from app.db.repository import Repository


class TestRepository(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        db_path = Path(self._tmp.name) / "test.db"
        self.conn = connect(db_path)
        self.repo = Repository(self.conn)

    def tearDown(self):
        self.conn.close()
        self._tmp.cleanup()

    def test_customer_get_or_create_is_idempotent(self):
        id1 = self.repo.get_or_create_customer("Acme Corp")
        id2 = self.repo.get_or_create_customer("Acme Corp")
        self.assertEqual(id1, id2)
        self.assertEqual(len(self.repo.list_customers()), 1)

    def test_call_lifecycle_and_note(self):
        cust_id = self.repo.get_or_create_customer("Globex")
        call_id = self.repo.create_call(cust_id, started_at="2026-03-01T10:00:00")
        self.repo.append_transcript(call_id, "Hello there.")
        self.repo.append_transcript(call_id, "Let's discuss the contract.")
        self.repo.finish_call(call_id, duration_seconds=120, audio_path="/tmp/x.wav")

        call = self.repo.get_call(call_id)
        self.assertEqual(call["status"], "done")
        self.assertEqual(call["transcript"], "Hello there. Let's discuss the contract.")
        self.assertEqual(call["customer_name"], "Globex")

        note_id = self.repo.add_note(call_id, "Discussed contract.", "Hi, ...", "[]")
        self.assertTrue(note_id)
        note = self.repo.get_note_for_call(call_id)
        self.assertEqual(note["summary"], "Discussed contract.")

    def test_tasks_extraction_and_completion(self):
        cust_id = self.repo.get_or_create_customer("Initech")
        call_id = self.repo.create_call(cust_id)
        task_id = self.repo.add_task(
            "Send updated pricing sheet",
            call_id=call_id,
            customer_id=cust_id,
            source="call",
        )
        open_tasks = self.repo.list_tasks(status="open")
        self.assertEqual(len(open_tasks), 1)
        self.assertEqual(open_tasks[0]["source"], "call")

        self.repo.complete_task(task_id)
        self.assertEqual(len(self.repo.list_tasks(status="open")), 0)
        self.assertEqual(len(self.repo.list_tasks(status="done")), 1)

    def test_due_tasks_query_for_reminders(self):
        self.repo.add_task("No due date task")
        self.repo.add_task("Overdue task", due_date="2020-01-01")
        self.repo.add_task("Future task", due_date="2099-01-01")

        due = self.repo.count_open_tasks_due("2026-07-28")
        descriptions = {r["description"] for r in due}
        self.assertIn("No due date task", descriptions)
        self.assertIn("Overdue task", descriptions)
        self.assertNotIn("Future task", descriptions)

    def test_yearly_stats(self):
        cust_id = self.repo.get_or_create_customer("Umbrella")
        call_id = self.repo.create_call(cust_id, started_at="2026-05-01T09:00:00")
        self.repo.finish_call(call_id, duration_seconds=600, audio_path=None)
        self.repo.add_note(call_id, "Great call.", "Email body", "[]")
        task_id = self.repo.add_task("Follow up", customer_id=cust_id)
        self.repo.complete_task(task_id)

        stats = self.repo.yearly_stats(2026)
        self.assertEqual(stats["call_count"], 1)
        self.assertEqual(stats["total_seconds"], 600)
        self.assertEqual(stats["tasks_completed"], 1)
        self.assertEqual(stats["top_customers"][0]["name"], "Umbrella")
        self.assertIn("Great call.", stats["summaries"])

        stats_other_year = self.repo.yearly_stats(2020)
        self.assertEqual(stats_other_year["call_count"], 0)


if __name__ == "__main__":
    unittest.main()
