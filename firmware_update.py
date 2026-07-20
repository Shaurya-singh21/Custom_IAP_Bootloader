import os
import sys
import time
import threading
import serial

#configure
PORT = "/dev/ttyACM0"           
BAUD_RATE = 115200
BINARY_PATH = "cli_app.bin"
CHUNK_SIZE = 256               

stop_flag = threading.Event()


def stm32_crc32(data: bytes) -> int:
    
    POLY = 0x04C11DB7
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        word = (data[i] | (data[i + 1] << 8) | (data[i + 2] << 16)
                | (data[i + 3] << 24))
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def process_and_pad_binary(file_path):
    if not os.path.exists(file_path):
        print(f"Error: Binary file '{file_path}' not found.")
        sys.exit(1)

    actual_size = os.path.getsize(file_path)
    with open(file_path, "rb") as f:
        raw_bytes = bytearray(f.read())

    packet_count = (actual_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    total_required_bytes = packet_count * CHUNK_SIZE

    if len(raw_bytes) < total_required_bytes:
        raw_bytes.extend([0xFF] * (total_required_bytes - len(raw_bytes)))

    return actual_size, packet_count, raw_bytes


def stdin_thread(ser):
    while not stop_flag.is_set():
        try:
            line = input()
        except EOFError:
            break
        ser.write((line + "\r").encode("utf-8"))
        ser.flush()


def main():
    actual_size, packet_count, padded_data = process_and_pad_binary(BINARY_PATH)
    calculated_crc = stm32_crc32(padded_data)
    print(f"Loaded firmware: {actual_size} bytes ({packet_count} packets after padding).")
    print(f"Pre-computed target verification CRC32 (MPEG-2): 0x{calculated_crc:08X}")

    try:
        ser = serial.Serial(PORT, BAUD_RATE, timeout=0.1)
    except serial.SerialException as e:
        print(f"Could not open {PORT}: {e}")
        sys.exit(1)

    reader = threading.Thread(target=stdin_thread, args=(ser,), daemon=True)
    reader.start()

    line_buffer = ""
    try:
        while True:
            if ser.in_waiting > 0:
                char = ser.read(1).decode("utf-8", errors="ignore")
                sys.stdout.write(char)
                sys.stdout.flush()
                line_buffer += char

                if "Getting file size..." in line_buffer:
                    time.sleep(0.05)
                    ser.reset_input_buffer()

                    print(f"\nInjecting file size: {actual_size} bytes...")
                    ser.write(actual_size.to_bytes(4, byteorder="big"))
                    ser.flush()
                    line_buffer = ""
                    
                elif "Successfully erased..." in line_buffer:
                    time.sleep(0.05)
                    ser.reset_input_buffer()

                    transfer_ok = True
                    for i in range(packet_count):
                        start = i * CHUNK_SIZE
                        chunk = padded_data[start:start + CHUNK_SIZE]
                        checksum = 0
                        for b in chunk:
                            checksum ^= b
                        ser.write(chunk + bytes([checksum]))
                        ser.flush()
                        ack = ser.read(1)
                        if ack != b'A':
                            print(f"\nTransmission failed at packet {i + 1}! Got: {ack}")
                            transfer_ok = False
                            break

                    if transfer_ok:
                        print("\nData injection complete.")
                    else:
                        print("\nData injection ABORTED — firmware is now waiting "
                              "on bytes that were never sent. Reset the board.")
                    line_buffer = ""
                elif "Checking for CRC value from script" in line_buffer:
                    time.sleep(0.05)
                    ser.reset_input_buffer()

                    print(f"\nInjecting validation CRC: 0x{calculated_crc:08X}...")
                    ser.write(calculated_crc.to_bytes(4, byteorder="big"))
                    ser.flush()
                    line_buffer = ""

                elif char in ("\r", "\n"):
                    line_buffer = ""

    except KeyboardInterrupt:
        print("\nScript terminated by user.")
    finally:
        stop_flag.set()
        if ser.is_open:
            ser.close()


if __name__ == "__main__":
    main()