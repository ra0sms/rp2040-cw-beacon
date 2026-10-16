#!/usr/bin/env python3
"""
Beacon GUI - PyQt5 configuration and control tool for the RP2040-Zero
CW/tone beacon firmware.

Talks to the beacon over its USB serial console. Allows editing all
parameters (message, CW tone, keying speed, tone sequence, transmission
mode, pause, transmission enable), applying them, storing them to flash and
watching the device log.

Requirements:
    pip install PyQt5 pyserial

Usage:
    python3 beacon_gui.py
"""

import re
import sys
from collections import deque

import serial
import serial.tools.list_ports

# Works with both PyQt5 and PyQt6 (whichever is installed)
try:
    import PyQt5.QtCore  # noqa: F401
    HAS_PYQT5 = True
except ImportError:
    HAS_PYQT5 = False

if HAS_PYQT5:
    from PyQt5.QtCore import QRegularExpression, QThread, pyqtSignal, QTimer
    from PyQt5.QtGui import (
        QColor, QRegularExpressionValidator, QTextCharFormat, QTextCursor,
    )
    from PyQt5.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QFormLayout, QGridLayout, QGroupBox,
        QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
        QPushButton, QSpinBox, QVBoxLayout, QWidget,
    )
else:
    from PyQt6.QtCore import QRegularExpression, QThread, pyqtSignal, QTimer
    from PyQt6.QtGui import (
        QColor, QRegularExpressionValidator, QTextCharFormat, QTextCursor,
    )
    from PyQt6.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QFormLayout, QGridLayout, QGroupBox,
        QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
        QPushButton, QSpinBox, QVBoxLayout, QWidget,
    )

BAUD_RATE = 115200  # irrelevant for USB CDC, but required by pyserial
ACK_TIMEOUT_MS = 1500  # max wait for the device's reply to a command

# Device replies that mean a command was rejected
ERROR_PREFIXES = ("Unknown", "usage:", "invalid", "error:")

# PyQt6 requires fully-qualified enum names
if HAS_PYQT5:
    CURSOR_END = QTextCursor.End
else:
    CURSOR_END = QTextCursor.MoveOperation.End


class SerialReader(QThread):
    """Background thread that reads lines from the serial port."""
    line_received = pyqtSignal(str)
    connection_lost = pyqtSignal(str)

    def __init__(self, ser: serial.Serial):
        super().__init__()
        self.ser = ser
        self._running = True

    def run(self):
        buf = b""
        while self._running:
            try:
                data = self.ser.read(256)
            except (serial.SerialException, OSError) as e:
                if self._running:  # not a regular disconnect: cable pulled etc.
                    self.connection_lost.emit(str(e))
                break
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", errors="replace").rstrip("\r")
                if text:
                    self.line_received.emit(text)
        # emit what is left
        if buf:
            self.line_received.emit(buf.decode("ascii", errors="replace"))

    def stop(self):
        self._running = False
        self.wait(2000)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.ser = None
        self.reader = None
        # Commands are sent one at a time: the next one goes out when the
        # device has answered the previous one (or after ACK_TIMEOUT_MS)
        self._queue = deque()
        self._pending = None
        self._batch = None  # {"fail": [...], "warn": [...]} while Apply runs
        self._ack_timer = QTimer(self)
        self._ack_timer.setSingleShot(True)
        self._ack_timer.timeout.connect(self._on_ack_timeout)

        self.setWindowTitle("Beacon GUI - RP2040 CW Beacon")
        self.resize(560, 800)
        self._build_ui()
        self.refresh_ports()

    # ------------------------------------------------------------------
    #  UI construction
    # ------------------------------------------------------------------
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # --- connection box ---
        conn_box = QGroupBox("Connection")
        conn = QHBoxLayout(conn_box)
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(170)
        btn_refresh = QPushButton("Refresh")
        btn_refresh.clicked.connect(self.refresh_ports)
        self.btn_connect = QPushButton("Connect")
        self.btn_connect.clicked.connect(self.toggle_connection)
        conn.addWidget(QLabel("Port:"))
        conn.addWidget(self.port_combo, 1)
        conn.addWidget(btn_refresh)
        conn.addWidget(self.btn_connect)
        root.addWidget(conn_box)
        self.ver_label = QLabel("Firmware: -")
        root.addWidget(self.ver_label)

        # --- parameters box ---
        param_box = QGroupBox("Beacon parameters")
        form = QFormLayout(param_box)
        form.setVerticalSpacing(4)
        form.setHorizontalSpacing(8)

        self.msg_edit = QLineEdit()
        self.msg_edit.setMaxLength(95)
        self.msg_edit.setToolTip(
            "Morse message: A-Z 0-9 . , ? / - = @ : + ( ) \" ' !\n"
            "Prosigns in angle brackets: <SK> <BT> <AR>")
        # Only characters the firmware can send (Latin letters, digits, punctuation)
        self.msg_edit.setValidator(QRegularExpressionValidator(
            QRegularExpression(r"[A-Za-z0-9 .,?/=@:+()\"'!<>-]*"), self.msg_edit))
        form.addRow("Message (CW):", self.msg_edit)

        self.tone_spin = QSpinBox()
        self.tone_spin.setRange(100, 5000)
        self.tone_spin.setValue(800)
        self.tone_spin.setSuffix(" Hz")
        self.tone_spin.setFixedWidth(110)
        form.addRow("CW tone:", self.tone_spin)

        self.wpm_spin = QSpinBox()
        self.wpm_spin.setRange(5, 60)
        self.wpm_spin.setValue(20)
        self.wpm_spin.setSuffix(" WPM")
        self.wpm_spin.setFixedWidth(110)
        form.addRow("Keying speed:", self.wpm_spin)

        self.mode_combo = QComboBox()
        self.mode_combo.addItems(["cw", "tones", "both"])
        self.mode_combo.setFixedWidth(110)
        form.addRow("TX mode:", self.mode_combo)

        self.tones_edit = QLineEdit()
        self.tones_edit.setToolTip(
            'Tone sequence "freq:ms" pairs, freq 0 = silence.\n'
            "Example: 800:150 0:100 800:150 0:100 1200:150 0:500")
        self.tones_edit.setValidator(QRegularExpressionValidator(
            QRegularExpression(r"[0-9: ]*"), self.tones_edit))
        form.addRow("Tone sequence:", self.tones_edit)

        self.pause_spin = QSpinBox()
        self.pause_spin.setRange(0, 3600)
        self.pause_spin.setValue(10)
        self.pause_spin.setSuffix(" s")
        self.pause_spin.setFixedWidth(110)
        form.addRow("Pause between repeats:", self.pause_spin)

        self.led_spin = QSpinBox()
        self.led_spin.setRange(0, 100)
        self.led_spin.setValue(20)
        self.led_spin.setSuffix(" %")
        self.led_spin.setFixedWidth(110)
        form.addRow("Status LED brightness:", self.led_spin)

        self.txen_check = QCheckBox("Transmission enabled")
        self.txen_check.setChecked(True)
        self.txen_check.setToolTip(
            "Uncheck to stop the beacon from transmitting (e.g. while tuning); "
            "check again to resume. Stored with 'Save to flash'.")
        form.addRow("", self.txen_check)

        root.addWidget(param_box)

        # --- actions ---
        actions = QGridLayout()
        self.btn_apply = QPushButton("Apply")
        self.btn_apply.setToolTip("Send all parameters to the device")
        self.btn_apply.clicked.connect(self.apply_params)
        self.btn_save = QPushButton("Save to flash")
        self.btn_save.setToolTip("Apply the fields, then store the configuration in flash")
        self.btn_save.clicked.connect(self.apply_and_save)
        self.btn_read = QPushButton("Read from device")
        self.btn_read.setToolTip("Request 'show' and fill the fields")
        self.btn_read.clicked.connect(lambda: self.send_command("show"))
        self.btn_defaults = QPushButton("Defaults")
        self.btn_defaults.clicked.connect(self.restore_defaults)

        actions.addWidget(self.btn_apply, 0, 0)
        actions.addWidget(self.btn_read, 0, 1)
        actions.addWidget(self.btn_save, 0, 2)
        actions.addWidget(self.btn_defaults, 1, 0)
        self.chk_tx_monitor = QCheckBox("Silence TX monitor output")
        self.chk_tx_monitor.setToolTip("Hide 'TX:' / 'TX done' lines in the log")
        actions.addWidget(self.chk_tx_monitor, 1, 1, 1, 2)
        root.addLayout(actions)

        # --- manual command ---
        cmd_box = QGroupBox("Manual command")
        cmd = QHBoxLayout(cmd_box)
        self.cmd_edit = QLineEdit()
        self.cmd_edit.setPlaceholderText("e.g. show, help, msg CQ DE RA0SMS ...")
        self.cmd_edit.returnPressed.connect(self.send_manual)
        btn_send = QPushButton("Send")
        btn_send.clicked.connect(self.send_manual)
        cmd.addWidget(self.cmd_edit, 1)
        cmd.addWidget(btn_send)
        root.addWidget(cmd_box)

        # --- log ---
        log_box = QGroupBox("Device log")
        log_layout = QVBoxLayout(log_box)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        self.log.setMinimumHeight(260)
        log_layout.addWidget(self.log)
        root.addWidget(log_box, 1)

        self._set_controls_enabled(False)

    # ------------------------------------------------------------------
    #  Serial handling
    # ------------------------------------------------------------------
    def refresh_ports(self):
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{p.device} - {p.description}", p.device)
        # prefer ttyACM (RP2040 CDC)
        for i in range(self.port_combo.count()):
            if "ACM" in self.port_combo.itemText(i):
                self.port_combo.setCurrentIndex(i)
                break

    def toggle_connection(self):
        if self.ser is None:
            self.connect_port()
        else:
            self.disconnect_port()

    def connect_port(self):
        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "Beacon GUI", "No serial port selected")
            return
        try:
            self.ser = serial.Serial(port, BAUD_RATE, timeout=0.05)
        except serial.SerialException as e:
            QMessageBox.critical(self, "Beacon GUI", f"Cannot open {port}:\n{e}")
            self.ser = None
            return

        self.reader = SerialReader(self.ser)
        self.reader.line_received.connect(self.on_device_line)
        self.reader.connection_lost.connect(self.on_connection_lost)
        self.reader.start()

        self.btn_connect.setText("Disconnect")
        self._set_controls_enabled(True)
        self._append_log(f"--- Connected to {port} ---", self.COLOR_INFO)
        # give the device a moment, then read its config.
        # QTimer.singleShot runs in the GUI thread - UI calls from foreign
        # threads (e.g. threading.Timer) crash Qt
        QTimer.singleShot(1000, lambda: self.send_command("show"))

    def disconnect_port(self, reason: str = None):
        if self.reader:
            self.reader.stop()
            self.reader = None
        if self.ser:
            try:
                self.ser.close()
            except (serial.SerialException, OSError):
                pass
            self.ser = None
        self._ack_timer.stop()
        self._queue.clear()
        self._pending = None
        self._batch = None
        self.btn_connect.setText("Connect")
        self._set_controls_enabled(False)
        self.ver_label.setText("Firmware: -")
        if reason:
            self._append_log(f"--- Connection lost: {reason} ---", self.COLOR_ERR)
        else:
            self._append_log("--- Disconnected ---", self.COLOR_INFO)

    def on_connection_lost(self, reason: str):
        if self.ser:
            self.disconnect_port(reason)
            self.refresh_ports()

    def send_command(self, cmd: str):
        """Queue a command; it is sent once the previous one is answered."""
        if not self.ser:
            QMessageBox.warning(self, "Beacon GUI", "Not connected")
            return
        try:
            cmd.encode("ascii")
        except UnicodeEncodeError:
            # The device console accepts ASCII only (e.g. no Cyrillic)
            self._append_log(f"[TX ERROR] non-ASCII characters in: {cmd}",
                             self.COLOR_ERR)
            return
        self._queue.append(cmd)
        self._pump()

    def _pump(self):
        while self._pending is None and self._queue and self.ser:
            cmd = self._queue.popleft()
            try:
                self.ser.write((cmd + "\n").encode("ascii"))
            except (serial.SerialException, OSError) as e:
                self._append_log(f"[TX ERROR] {e}", self.COLOR_ERR)
                self._note_failure(cmd, str(e))
                continue
            self._append_log(f"> {cmd}", self.COLOR_CMD)
            self._pending = cmd
            self._ack_timer.start(ACK_TIMEOUT_MS)
        self._finish_batch()

    def _check_ack(self, line: str):
        """Treat the first real reply to the pending command as its answer."""
        if self._batch is not None and line.lower().startswith("warning"):
            self._batch["warn"].append(line)
        if self._pending is None:
            return
        # Skip the echo of the command itself, TX monitor and warning lines
        if (line == self._pending or line.startswith("TX")
                or line.lower().startswith("warning")):
            return
        if line.startswith(ERROR_PREFIXES):
            self._note_failure(self._pending, line)
        self._ack_timer.stop()
        self._pending = None
        self._pump()

    def _on_ack_timeout(self):
        if self._pending is not None:
            self._append_log(f"[NO REPLY] {self._pending}", self.COLOR_ERR)
            self._note_failure(self._pending, "no reply from the device")
            self._pending = None
        self._pump()

    def _note_failure(self, cmd: str, reason: str):
        if self._batch is not None:
            self._batch["fail"].append(f"{cmd}  →  {reason}")

    def _finish_batch(self):
        """After Apply/Save: report rejected commands and warnings."""
        if self._batch is None or self._pending is not None or self._queue:
            return
        batch, self._batch = self._batch, None
        if batch["fail"] or batch["warn"]:
            text = ""
            if batch["fail"]:
                text += "Not accepted by the device:\n" + "\n".join(batch["fail"])
            if batch["warn"]:
                text += ("\n\n" if text else "") + "\n".join(batch["warn"])
            QMessageBox.warning(self, "Beacon GUI", text)

    def send_manual(self):
        cmd = self.cmd_edit.text().strip()
        if cmd:
            self.send_command(cmd)
            self.cmd_edit.clear()

    # ------------------------------------------------------------------
    #  Device output handling
    # ------------------------------------------------------------------
    # Colors for the device log (readable on both light and dark themes)
    COLOR_CMD = "#005f87"      # commands sent by the GUI
    COLOR_INFO = "#4080c0"     # connection headers
    COLOR_OK = "#007a3d"       # confirmations (set / saved / loaded)
    COLOR_TX = "#b05a00"       # TX monitor lines
    COLOR_ERR = "#cc0000"      # errors

    def _append_log(self, text: str, color: str = None):
        """Append a line to the log, optionally colored."""
        cursor = self.log.textCursor()
        cursor.movePosition(CURSOR_END)
        fmt = QTextCharFormat()
        if color:
            fmt.setForeground(QColor(color))
        cursor.insertText(text + "\n", fmt)
        self.log.setTextCursor(cursor)
        self.log.ensureCursorVisible()

    def on_device_line(self, line: str):
        if line == self._pending:
            return  # device echo of the command, already logged as "> cmd"
        is_tx = line.startswith(("TX:", "TX done", "TX aborted"))
        if self.chk_tx_monitor.isChecked() and is_tx:
            return
        if is_tx:
            color = self.COLOR_TX
        elif ("set:" in line or "saved" in line or "restored" in line
                or "loaded from flash" in line):
            color = self.COLOR_OK
        elif (line.startswith(ERROR_PREFIXES) or line.startswith("[TX ERROR]")
                or line.lower().startswith("warning")):
            color = self.COLOR_ERR
        else:
            color = None
        self._append_log(line, color)
        self._parse_config_line(line)
        self._check_ack(line)

    def _parse_config_line(self, line: str):
        """Extract values from the 'show' output into the UI fields."""
        try:
            m = re.match(r"mode\s*:\s*(\w+)", line)
            if m:
                idx = self.mode_combo.findText(m.group(1))
                if idx >= 0:
                    self.mode_combo.setCurrentIndex(idx)
                return
            m = re.match(r"tone\s*:\s*(\d+)", line)
            if m:
                self.tone_spin.setValue(int(m.group(1)))
                return
            m = re.match(r"msg\s*:\s*(.*)$", line)
            if m:
                self.msg_edit.setText(m.group(1))
                return
            m = re.match(r"tones\s*:\s*(.*)$", line)
            if m:
                self.tones_edit.setText("" if m.group(1) == "(empty)" else m.group(1))
                return
            m = re.match(r"pause\s*:\s*(\d+)", line)
            if m:
                self.pause_spin.setValue(int(m.group(1)))
                return
            m = re.match(r"wpm\s*:\s*(\d+)", line)
            if m:
                self.wpm_spin.setValue(int(m.group(1)))
                return
            m = re.match(r"led\s*:\s*(\d+)", line)
            if m:
                self.led_spin.setValue(int(m.group(1)))
                return
            m = re.match(r"ver\s*:\s*(.*)$", line)
            if m:
                self.ver_label.setText(f"Firmware: {m.group(1)}")
                return
            m = re.match(r"txen\s*:\s*(\d)", line)
            if m:
                self.txen_check.setChecked(int(m.group(1)) == 1)
        except ValueError:
            pass

    # ------------------------------------------------------------------
    #  Actions
    # ------------------------------------------------------------------
    def apply_params(self):
        mode = self.mode_combo.currentText()
        tones = self.tones_edit.text().strip()
        if mode in ("tones", "both") and not tones:
            QMessageBox.warning(
                self, "Beacon GUI",
                "Tone sequence is empty, so nothing will be transmitted in "
                "'tones'/'both' mode.\n\n"
                "Fill the 'Tone sequence' field, e.g.:\n"
                "800:150 0:100 800:150 0:500")
        self._batch = {"fail": [], "warn": []}
        cmds = [
            f"msg {self.msg_edit.text().strip()}" if self.msg_edit.text().strip() else None,
            f"tone {self.tone_spin.value()}",
            f"wpm {self.wpm_spin.value()}",
            f"txmode {mode}",
            f"tones {tones}" if tones else None,
            f"pause {self.pause_spin.value()}",
            f"led {self.led_spin.value()}",
            f"txen {1 if self.txen_check.isChecked() else 0}",
        ]
        for cmd in cmds:
            if cmd:
                self.send_command(cmd)

    def apply_and_save(self):
        """Apply the GUI fields to the device, then store them in flash."""
        self.apply_params()
        self.send_command("save")  # queued: sent after all fields are accepted

    def restore_defaults(self):
        self.send_command("default")
        self.send_command("show")  # refresh the fields

    def _set_controls_enabled(self, enabled: bool):
        for w in (self.msg_edit, self.tone_spin, self.wpm_spin, self.mode_combo,
                  self.tones_edit, self.pause_spin, self.led_spin, self.txen_check,
                  self.btn_apply, self.btn_save, self.btn_read,
                  self.btn_defaults):
            w.setEnabled(enabled)

    def closeEvent(self, event):
        self.disconnect_port()
        event.accept()


def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    # PyQt5 has exec_(), PyQt6 has exec()
    sys.exit(app.exec() if hasattr(app, "exec") else app.exec_())


if __name__ == "__main__":
    main()
