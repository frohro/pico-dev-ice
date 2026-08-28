import serial, time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
ser.write(b"VER\r\n")
time.sleep(0.1)
print("VER response:", ser.read_all().decode(errors='replace'))

ser.write(b"WIFI?\r\n")
time.sleep(0.1)
print("WIFI response:", ser.read_all().decode(errors='replace'))

ser.close()
