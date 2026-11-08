"""Open the Tab5's USB serial port without resetting it.

The ESP32-P4's built-in USB serial resets the chip on DTR/RTS transitions,
which pyserial's defaults trigger on open and close.
"""
import serial


def open_port(port, timeout=1.0):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = timeout
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser
