import socket
import threading
import sys
import os
import platform
import time

PORT = 3169
BUFFER_SIZE = 4096

# Get current user's home directory and set data file path
home_dir = os.path.expanduser("~")
data_file = os.path.join(home_dir, "passwd", "data.txt")

def get_device_name():
    return platform.node()

def get_all_interfaces():
    import psutil
    interfaces = []
    addrs = psutil.net_if_addrs()
    for iface, snics in addrs.items():
        for snic in snics:
            if snic.family == socket.AF_INET and not snic.address.startswith('127.'):
                interfaces.append((iface, snic.address))
    return interfaces if interfaces else [("lo", "127.0.0.1")]

def import_mode():
    print("Starting import mode. Listening for data...")

    # Show local interfaces and IPs
    interfaces = get_all_interfaces()
    print("\nAvailable Interfaces:")
    for iface, ip in interfaces:
        print(f" - {iface}: {ip}")
    print()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('', PORT))

    while True:
        data, addr = sock.recvfrom(BUFFER_SIZE)
        msg = data.decode(errors='ignore')

        if msg == "DISCOVER":
            ips = [ip for _, ip in get_all_interfaces()]
            ips_str = ';'.join(ips)
            reply = f"NAME:{get_device_name()},IPS:{ips_str}"
            sock.sendto(reply.encode(), addr)

        elif msg.startswith("DATA_START"):
            print(f"Incoming data transfer request from {addr[0]}.")
            while True:
                accept = input("Accept data? (y/n): ").strip().lower()
                if accept == 'y':
                    # Receive data
                    received = []
                    print("Receiving data...")
                    while True:
                        chunk, _ = sock.recvfrom(BUFFER_SIZE)
                        chunk_msg = chunk.decode(errors='ignore')
                        if chunk_msg == "DATA_END":
                            break
                        received.append(chunk)
                    os.makedirs(os.path.dirname(data_file), exist_ok=True)
                    with open(data_file, 'wb') as f:
                        for part in received:
                            f.write(part)
                    print(f"Data saved to {data_file}")
                    print(f"Restart app please c:")
                    break
                elif accept == 'n':
                    print("Data rejected.")
                    break
                else:
                    print("Please enter 'y' or 'n'.")

def export_mode():
    print("Starting export mode.")

    target_ip = input("Enter target device IP address: ").strip()
    if not target_ip:
        print("No IP entered. Cancelled.")
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        sock.settimeout(3)
        sock.sendto(b"DISCOVER", (target_ip, PORT))
        data, addr = sock.recvfrom(BUFFER_SIZE)
        print(f"Device response: {data.decode(errors='ignore')}")
    except Exception:
        print("No response from device or device offline.")
        return

    if not os.path.exists(data_file):
        print(f"Data file not found: {data_file}")
        return

    with open(data_file, 'rb') as f:
        data = f.read()

    sock.sendto(b"DATA_START", (target_ip, PORT))

    chunk_size = 1024
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i+chunk_size]
        sock.sendto(chunk, (target_ip, PORT))
        time.sleep(0.01)

    sock.sendto(b"DATA_END", (target_ip, PORT))
    print("Data transfer complete.")

def main():
    print("Select mode:")
    print("1. Export data")
    print("2. Import data")
    choice = input("Choice: ").strip()
    if choice == '1':
        export_mode()
    elif choice == '2':
        import_mode()
    else:
        print("Invalid choice.")

if __name__ == "__main__":
    try:
        import psutil
    except ImportError:
        print("Module 'psutil' required. Install with: pip install psutil")
        sys.exit(1)
    main()
