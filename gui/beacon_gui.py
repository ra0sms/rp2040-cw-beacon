#!/usr/bin/env python3
"""
Beacon GUI - PyQt5 configuration and control tool for the RP2040-Zero
CW/tone beacon firmware.

Talks to the beacon over its USB serial console. Allows editing all
parameters (message, CW tone, keying speed, tone sequence, transmission
mode, pause, PTT polarity), applying them, storing them to flash and
watching the device log.

Requirements:
    pip install PyQt5 pyserial

Usage:
    python3 beacon_gui.py
"""

import re
import sys
import threading
import time

import serial
import serial.tools.list_ports

# Works with both PyQt5 and PyQt6 (whichever is installed)
try:
    import PyQt5.QtCore  # noqa: F401
    HAS_PYQT5 = True
except ImportError:
    HAS_PYQT5 = False

if HAS_PYQT5:
    from PyQt5.QtCore import QThread, pyqtSignal, QTimer
    from PyQt5.QtGui import QColor, QTextCharFormat, QTextCursor
    from PyQt5.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QFormLayout, QGridLayout, QGroupBox,
        QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
        QPushButton, QSpinBox, QVBoxLayout, QWidget,
    )
else:
    from PyQt6.QtCore import QThread, pyqtSignal, QTimer
    from PyQt6.QtGui import QColor, QTextCharFormat, QTextCursor
    from PyQt6.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QFormLayout, QGridLayout, QGroupBox,
        QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
        QPushButton, QSpinBox, QVBoxLayout, QWidget,
    )

BAUD_RATE = 115200  # irrelevant for USB CDC, but required by pyserial

# PyQt6 requires fully-qualified enum names
if HAS_PYQT5:
    CURSOR_END = QTextCursor.End
else:
    CURSOR_END = QTextCursor.MoveOperation.End


class SerialReader(QThread):
    """Background thread that reads lines from the serial port."""
    line_received = pyqtSignal(str)

    def __init__(self, ser: serial.Serial):
        super().__init__()
        self.ser = ser
        self._running = True

    def run(self):
        buf = b""
        while self._running:
            try:
                data = self.ser.read(256)
            except (serial.SerialException, OSError):
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
        self._tx_lock = threading.Lock()

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

        # --- parameters box ---
        param_box = QGroupBox("Beacon parameters")
        form = QFormLayout(param_box)
        form.setVerticalSpacing(4)
        form.setHorizontalSpacing(8)

        self.msg_edit = QLineEdit()
        self.msg_edit.setMaxLength(95)
        self.msg_edit.setToolTip("Morse message: A-Z 0-9 . , ? / - =")
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
        form.addRow("Tone sequence:", self.tones_edit)

        self.pause_spin = QSpinBox()
        self.pause_spin.setRange(0, 3600)
        self.pause_spin.setValue(10)
        self.pause_spin.setSuffix(" s")
        self.pause_spin.setFixedWidth(110)
        form.addRow("Pause between repeats:", self.pause_spin)

        self.ptt_combo = QComboBox()
        self.ptt_combo.addItems(["1 (active high)", "0 (active low)"])
        self.ptt_combo.setFixedWidth(140)
        form.addRow("PTT active level:", self.ptt_combo)

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
        self.btn_save.setToolTip("Store current configuration in flash")
        self.btn_save.clicked.connect(lambda: self.send_command("save"))
        self.btn_read = QPushButton("Read from device")
        self.btn_read.setToolTip("Request 'show' and fill the fields")
        self.btn_read.clicked.connect(lambda: self.send_command("show"))
        self.btn_defaults = QPushButton("Defaults")
        self.btn_defaults.clicked.connect(lambda: self.send_command("default"))
        self.btn_stop = QPushButton("Stop TX (pause=0)")
        self.btn_stop.setToolTip("Pause=0 makes the beacon repeat continuously")
        self.btn_stop.clicked.connect(lambda: self.send_command("pause 0"))

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
        self.reader.start()

        self.btn_connect.setText("Disconnect")
        self._set_controls_enabled(True)
        self._append_log(f"--- Connected to {port} ---", self.COLOR_INFO)
        # give the device a moment, then read its config.
        # QTimer.singleShot runs in the GUI thread - UI calls from foreign
        # threads (e.g. threading.Timer) crash Qt
        QTimer.singleShot(1000, lambda: self.send_command("show"))

    def disconnect_port(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
        if self.ser:
            self.ser.close()
            self.ser = None
        self.btn_connect.setText("Connect")
        self._set_controls_enabled(False)
        self._append_log("--- Disconnected ---", self.COLOR_INFO)

    def send_command(self, cmd: str):
        if not self.ser:
            QMessageBox.warning(self, "Beacon GUI", "Not connected")
            return
        with self._tx_lock:
            try:
                self.ser.write((cmd + "\n").encode("ascii"))
            except serial.SerialException as e:
                self._append_log(f"[TX ERROR] {e}", self.COLOR_ERR)
        self._append_log(f"> {cmd}", self.COLOR_CMD)

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
        if self.chk_tx_monitor.isChecked() and (
                line.startswith("TX:") or line.startswith("TX done")):
            return
        if line.startswith("TX:") or line.startswith("TX done"):
            color = self.COLOR_TX
        elif ("set:" in line or "saved" in line or "restored" in line
                or "loaded from flash" in line):
            color = self.COLOR_OK
        elif (line.startswith("Unknown") or line.startswith("[TX ERROR]")
                or line.startswith("usage:") or line.startswith("invalid")):
            color = self.COLOR_ERR
        else:
            color = None
        self._append_log(line, color)
        self._parse_config_line(line)

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
            m = re.match(r"ptt\s*:\s*GP\d+,\s*active level\s*(\d)", line)
            if m:
                self.ptt_combo.setCurrentIndex(int(m.group(1)))
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
        cmds = [
            f"msg {self.msg_edit.text().strip()}" if self.msg_edit.text().strip() else None,
            f"tone {self.tone_spin.value()}",
            f"wpm {self.wpm_spin.value()}",
            f"txmode {mode}",
            f"tones {self.tones_edit.text().strip()}"
            if (mode in ("tones", "both") and self.tones_edit.text().strip()) else None,
            f"pause {self.pause_spin.value()}",
            f"ptt {self.ptt_combo.currentIndex()}",
            f"txen {1 if self.txen_check.isChecked() else 0}",
        ]
        for cmd in cmds:
            if cmd:
                self.send_command(cmd)
                time.sleep(0.05)  # let the device process commands in order

    def _set_controls_enabled(self, enabled: bool):
        for w in (self.msg_edit, self.tone_spin, self.wpm_spin, self.mode_combo,
                  self.tones_edit, self.pause_spin, self.ptt_combo, self.txen_check,
                  self.btn_apply, self.btn_save, self.btn_read,
                  self.btn_defaults, self.btn_stop):
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
