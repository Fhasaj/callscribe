import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.llm.prompts import build_summary_prompt, build_yearly_narrative_prompt


class TestPrompts(unittest.TestCase):
    def test_summary_prompt_includes_context_and_json_instructions(self):
        prompt = build_summary_prompt("We discussed pricing.", "Acme Corp", "2026-07-28")
        self.assertIn("Acme Corp", prompt)
        self.assertIn("2026-07-28", prompt)
        self.assertIn("We discussed pricing.", prompt)
        self.assertIn('"summary"', prompt)
        self.assertIn('"customer_email"', prompt)
        self.assertIn('"tasks"', prompt)

    def test_summary_prompt_handles_empty_transcript(self):
        prompt = build_summary_prompt("", "", "2026-07-28")
        self.assertIn("no speech detected", prompt)
        self.assertIn("Unknown", prompt)

    def test_yearly_prompt_includes_stats_and_summaries(self):
        stats = {
            "call_count": 12,
            "total_seconds": 7200,
            "tasks_completed": 5,
            "top_customers": [{"name": "Acme", "n": 4}],
        }
        prompt = build_yearly_narrative_prompt(2026, ["Discussed renewal."], stats)
        self.assertIn("2026", prompt)
        self.assertIn("Total calls: 12", prompt)
        self.assertIn("2.0 hours", prompt)
        self.assertIn("Acme", prompt)
        self.assertIn("Discussed renewal.", prompt)


if __name__ == "__main__":
    unittest.main()
