#!/usr/bin/env python3
"""通过 RTS/DTR 引脚组合复位 ESP32-S3（不进入下载模式）。"""
import glob
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob("/dev/ttyACM*"))[0]
ser = serial.Serial()
ser.port = port
ser.open()
ser.setDTR(False)
ser.setRTS(True)   # EN 拉低
time.sleep(0.1)
ser.setRTS(False)  # EN 释放，复位完成
time.sleep(0.1)
ser.close()
print("reset done on", port)
