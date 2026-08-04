"""Thread-safe CRUD access to the SQLite DB. Plain parameterized SQL,
no ORM -- the schema is small and stable (see database.py)."""
from __future__ import annotations

import datetime as dt
import sqlite3
import threading
from typing import Any, Optional


def _now_iso() -> str:
    return dt.datetime.now().isoformat(timespec="seconds")


class Repository:
    def __init__(self, conn: sqlite3.Connection):
        self._conn = conn
        self._lock = threading.Lock()

    # -- customers --------------------------------------------------

    def get_or_create_customer(self, name: str) -> int:
        name = name.strip()
        with self._lock:
            cur = self._conn.execute("SELECT id FROM customers WHERE name = ?", (name,))
            row = cur.fetchone()
            if row:
                return row["id"]
            cur = self._conn.execute(
                "INSERT INTO customers (name, created_at) VALUES (?, ?)",
                (name, _now_iso()),
            )
            self._conn.commit()
            return cur.lastrowid

    def list_customers(self) -> list[sqlite3.Row]:
        with self._lock:
            return self._conn.execute(
                "SELECT * FROM customers ORDER BY name COLLATE NOCASE"
            ).fetchall()

    # -- calls --------------------------------------------------------

    def create_call(self, customer_id: Optional[int], started_at: Optional[str] = None) -> int:
        with self._lock:
            cur = self._conn.execute(
                "INSERT INTO calls (customer_id, started_at, status) VALUES (?, ?, 'recording')",
                (customer_id, started_at or _now_iso()),
            )
            self._conn.commit()
            return cur.lastrowid

    def set_call_status(self, call_id: int, status: str) -> None:
        with self._lock:
            self._conn.execute("UPDATE calls SET status = ? WHERE id = ?", (status, call_id))
            self._conn.commit()

    def append_transcript(self, call_id: int, text_chunk: str) -> None:
        with self._lock:
            row = self._conn.execute(
                "SELECT transcript FROM calls WHERE id = ?", (call_id,)
            ).fetchone()
            existing = (row["transcript"] or "") if row else ""
            updated = (existing + " " + text_chunk).strip() if existing else text_chunk
            self._conn.execute(
                "UPDATE calls SET transcript = ? WHERE id = ?", (updated, call_id)
            )
            self._conn.commit()

    def finish_call(
        self,
        call_id: int,
        duration_seconds: int,
        audio_path: Optional[str],
        status: str = "done",
    ) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE calls SET ended_at = ?, duration_seconds = ?, audio_path = ?, status = ? "
                "WHERE id = ?",
                (_now_iso(), duration_seconds, audio_path, status, call_id),
            )
            self._conn.commit()

    def get_call(self, call_id: int) -> Optional[sqlite3.Row]:
        with self._lock:
            return self._conn.execute(
                "SELECT calls.*, customers.name AS customer_name FROM calls "
                "LEFT JOIN customers ON customers.id = calls.customer_id "
                "WHERE calls.id = ?",
                (call_id,),
            ).fetchone()

    def list_calls(
        self, customer_name: Optional[str] = None, search: Optional[str] = None
    ) -> list[sqlite3.Row]:
        query = (
            "SELECT calls.*, customers.name AS customer_name FROM calls "
            "LEFT JOIN customers ON customers.id = calls.customer_id WHERE 1=1"
        )
        params: list[Any] = []
        if customer_name:
            query += " AND customers.name = ?"
            params.append(customer_name)
        if search:
            query += " AND (calls.transcript LIKE ? OR customers.name LIKE ?)"
            like = f"%{search}%"
            params.extend([like, like])
        query += " ORDER BY calls.started_at DESC"
        with self._lock:
            return self._conn.execute(query, params).fetchall()

    # -- notes --------------------------------------------------------

    def add_note(
        self, call_id: int, summary: str, email_draft: str, key_points_json: str
    ) -> int:
        with self._lock:
            cur = self._conn.execute(
                "INSERT INTO notes (call_id, summary, email_draft, key_points_json, created_at) "
                "VALUES (?, ?, ?, ?, ?)",
                (call_id, summary, email_draft, key_points_json, _now_iso()),
            )
            self._conn.commit()
            return cur.lastrowid

    def get_note_for_call(self, call_id: int) -> Optional[sqlite3.Row]:
        with self._lock:
            return self._conn.execute(
                "SELECT * FROM notes WHERE call_id = ? ORDER BY id DESC LIMIT 1", (call_id,)
            ).fetchone()

    # -- tasks --------------------------------------------------------

    def add_task(
        self,
        description: str,
        call_id: Optional[int] = None,
        customer_id: Optional[int] = None,
        due_date: Optional[str] = None,
        source: str = "manual",
    ) -> int:
        with self._lock:
            cur = self._conn.execute(
                "INSERT INTO tasks (call_id, customer_id, description, due_date, status, "
                "source, created_at) VALUES (?, ?, ?, ?, 'open', ?, ?)",
                (call_id, customer_id, description, due_date, source, _now_iso()),
            )
            self._conn.commit()
            return cur.lastrowid

    def list_tasks(
        self, status: Optional[str] = None, customer_name: Optional[str] = None
    ) -> list[sqlite3.Row]:
        query = (
            "SELECT tasks.*, customers.name AS customer_name FROM tasks "
            "LEFT JOIN customers ON customers.id = tasks.customer_id WHERE 1=1"
        )
        params: list[Any] = []
        if status:
            query += " AND tasks.status = ?"
            params.append(status)
        if customer_name:
            query += " AND customers.name = ?"
            params.append(customer_name)
        query += " ORDER BY (tasks.due_date IS NULL), tasks.due_date, tasks.created_at DESC"
        with self._lock:
            return self._conn.execute(query, params).fetchall()

    def complete_task(self, task_id: int) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE tasks SET status = 'done', completed_at = ? WHERE id = ?",
                (_now_iso(), task_id),
            )
            self._conn.commit()

    def reopen_task(self, task_id: int) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE tasks SET status = 'open', completed_at = NULL WHERE id = ?",
                (task_id,),
            )
            self._conn.commit()

    def delete_task(self, task_id: int) -> None:
        with self._lock:
            self._conn.execute("DELETE FROM tasks WHERE id = ?", (task_id,))
            self._conn.commit()

    def count_open_tasks_due(self, on_or_before: str) -> list[sqlite3.Row]:
        with self._lock:
            return self._conn.execute(
                "SELECT * FROM tasks WHERE status = 'open' AND "
                "(due_date IS NULL OR due_date <= ?) ORDER BY due_date IS NULL, due_date",
                (on_or_before,),
            ).fetchall()

    # -- yearly summary -------------------------------------------------

    def yearly_stats(self, year: int) -> dict:
        start, end = f"{year}-01-01", f"{year + 1}-01-01"
        with self._lock:
            call_row = self._conn.execute(
                "SELECT COUNT(*) AS n, COALESCE(SUM(duration_seconds), 0) AS secs FROM calls "
                "WHERE started_at >= ? AND started_at < ?",
                (start, end),
            ).fetchone()
            top_customers = self._conn.execute(
                "SELECT customers.name AS name, COUNT(*) AS n FROM calls "
                "JOIN customers ON customers.id = calls.customer_id "
                "WHERE calls.started_at >= ? AND calls.started_at < ? "
                "GROUP BY customers.id ORDER BY n DESC LIMIT 10",
                (start, end),
            ).fetchall()
            tasks_done = self._conn.execute(
                "SELECT COUNT(*) AS n FROM tasks WHERE status = 'done' "
                "AND completed_at >= ? AND completed_at < ?",
                (start, end),
            ).fetchone()
            summaries = self._conn.execute(
                "SELECT notes.summary FROM notes JOIN calls ON calls.id = notes.call_id "
                "WHERE calls.started_at >= ? AND calls.started_at < ? "
                "ORDER BY calls.started_at",
                (start, end),
            ).fetchall()
        return {
            "call_count": call_row["n"],
            "total_seconds": call_row["secs"],
            "top_customers": [dict(r) for r in top_customers],
            "tasks_completed": tasks_done["n"],
            "summaries": [r["summary"] for r in summaries if r["summary"]],
        }
