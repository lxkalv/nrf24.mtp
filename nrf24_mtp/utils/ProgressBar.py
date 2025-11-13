import shutil
import sys
import itertools

from . import Logger

# Simple spinner and global iterator (keeps visual continuity)
SPINNER = "⣾⣽⣻⢿⡿⣟⣯⣷"
_spinner = itertools.cycle(SPINNER)


class ProgressBar:
    """Minimal, terminal-focused progress bar.

    Designed to be simple and easy to use in your controlled terminal
    environment. Methods return the rendered string for easier testing.
    """

    TRAILING_MARGIN = 9

    def __init__(self: "ProgressBar", total: int | None = None) -> None:
        self.total    = total
        self._spinner = _spinner
        return
    
    def status(self, message, status):
        spin = next(self._spinner)
        rendered = self._format_status(message, status, spin)
        self._reset_line()
        if status == "INFO":
            Logger.INFO(rendered, end = "")
            sys.stdout.flush()
        elif status == "WARN":
            Logger.WARN(rendered, end = "")
            sys.stdout.flush()
        elif status == "SUCC":
            Logger.SUCC(rendered)
        elif status == "ERROR":
            Logger.ERROR(rendered)
        return rendered

    def update(self: "ProgressBar", current: int, pending_msg: str = "", finished_msg: str = ""):
        """Update the bar; returns the rendered string."""
        spin = next(self._spinner)
        rendered = self._format_progress(pending_msg, finished_msg, current, self.total or current, spin)

        self._reset_line()
        if self.total is None:
            Logger.INFO(rendered, end = "")
            sys.stdout.flush()
        
        else:
            if current < self.total:
                Logger.INFO(rendered, end = "")
                sys.stdout.flush()
            
            else:
                Logger.SUCC(rendered)

        return rendered
    
    def finish(self: "ProgressBar", finished_msg: str = ""):
        spin = next(self._spinner)
        rendered = self._format_progress("", finished_msg, self.total or 0, self.total or 0, spin)
        self._reset_line()
        Logger.SUCC(rendered)
        return rendered

    def _format_progress(self, pending_msg, finished_msg, current, total, spin):
        w = self._terminal_width()
        if total is None or total <= 0:
            return f"{pending_msg} ({current}/{total})"

        if current < total:
            progress = f"({current}/{total}) {spin}"
            return f"{pending_msg} {progress.rjust(w - self.TRAILING_MARGIN - len(pending_msg))}"
        else:
            progress = f"({current}/{total}) █"
            return f"{finished_msg} {progress.rjust(w - self.TRAILING_MARGIN - len(finished_msg))}"

    def _format_status(self, message, status, spin):
        w = self._terminal_width()
        if status in ("INFO", "WARN"):
            symbol = spin
        elif status == "SUCC":
            symbol = "█"
        elif status == "ERROR":
            symbol = "X"
        else:
            symbol = spin
        return f"{message} {symbol.rjust(w - self.TRAILING_MARGIN - len(message))}"

    def _terminal_width(self: "ProgressBar") -> int:
        return shutil.get_terminal_size().columns

    def _reset_line(self: "ProgressBar") -> None:
        print("\x1b[2K\r", end="")
        return