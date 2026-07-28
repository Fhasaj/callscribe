"""Tasks tab: open/completed tasks (manual + auto-extracted from
calls), checkable to complete, with a manual add row."""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QVBoxLayout,
    QWidget,
)


class TasksTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self._tasks: list = []

        layout = QVBoxLayout(self)

        add_row = QHBoxLayout()
        self.new_task_edit = QLineEdit()
        self.new_task_edit.setPlaceholderText("New task description...")
        self.new_task_edit.returnPressed.connect(self._add_task)
        add_row.addWidget(self.new_task_edit, 2)
        self.due_edit = QLineEdit()
        self.due_edit.setPlaceholderText("Due YYYY-MM-DD (optional)")
        self.due_edit.setFixedWidth(180)
        add_row.addWidget(self.due_edit)
        add_btn = QPushButton("Add Task")
        add_btn.clicked.connect(self._add_task)
        add_row.addWidget(add_btn)
        layout.addLayout(add_row)

        filter_row = QHBoxLayout()
        self.show_done_check = QCheckBox("Show completed")
        self.show_done_check.stateChanged.connect(self.refresh)
        filter_row.addWidget(self.show_done_check)
        filter_row.addStretch()
        delete_btn = QPushButton("Delete selected")
        delete_btn.clicked.connect(self._delete_selected)
        filter_row.addWidget(delete_btn)
        layout.addLayout(filter_row)

        self.task_list = QListWidget()
        self.task_list.itemChanged.connect(self._on_item_changed)
        layout.addWidget(self.task_list)

        self.refresh()

    def refresh(self) -> None:
        status_filter = None if self.show_done_check.isChecked() else "open"
        self._tasks = self.ctx.repo.list_tasks(status=status_filter)
        self.task_list.blockSignals(True)
        self.task_list.clear()
        for task in self._tasks:
            item = QListWidgetItem()
            item.setData(Qt.UserRole, task["id"])
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(Qt.Checked if task["status"] == "done" else Qt.Unchecked)
            due = f"  (due {task['due_date']})" if task["due_date"] else ""
            source = "  [from call]" if task["source"] == "call" else ""
            customer = f"  -- {task['customer_name']}" if task["customer_name"] else ""
            item.setText(f"{task['description']}{customer}{due}{source}")
            self.task_list.addItem(item)
        self.task_list.blockSignals(False)

    def _on_item_changed(self, item: QListWidgetItem) -> None:
        task_id = item.data(Qt.UserRole)
        if item.checkState() == Qt.Checked:
            self.ctx.repo.complete_task(task_id)
        else:
            self.ctx.repo.reopen_task(task_id)
        if not self.show_done_check.isChecked():
            self.refresh()

    def _add_task(self) -> None:
        description = self.new_task_edit.text().strip()
        if not description:
            return
        due = self.due_edit.text().strip() or None
        self.ctx.repo.add_task(description, due_date=due)
        self.new_task_edit.clear()
        self.due_edit.clear()
        self.refresh()

    def _delete_selected(self) -> None:
        for item in self.task_list.selectedItems():
            self.ctx.repo.delete_task(item.data(Qt.UserRole))
        self.refresh()
