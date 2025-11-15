from pathlib import Path
import sys

from nrf24_mtp.layers import ApplicationLayer

print("parse_arguments() demo:")
backup_argv = sys.argv.copy()
sys.argv = [
    "test_suite/layers_ApplicationLayer.py",
    "--mode", "TX",
    "--file-path-tx", "data/to_send.txt",
    "--print-config",
]
ApplicationLayer.parse_arguments("Demo")
sys.argv = backup_argv
print("\n\n")

print("get_usb_mount_path() demo:")
print(ApplicationLayer.get_usb_mount_path())
print("\n\n")

print("find_valid_txt_file_in_usb() demo:")
print(ApplicationLayer.find_valid_txt_file_in_usb(ApplicationLayer.get_usb_mount_path()))
print("\n\n")

print("load_file_bytes() demo:")
print("Path not provided:")
file = ApplicationLayer.load_file_bytes(None)
print(len(file), "bytes loaded")
print("\nPath provided:")
file = ApplicationLayer.load_file_bytes(Path("test_files/lorem.txt"))
print(len(file), "bytes loaded")
print("\nFile not found:")
file = ApplicationLayer.load_file_bytes(Path("non_existent_file.txt"))
print(file, "bytes loaded")
print("\n\n")

print("store_file_bytes() demo:")
data = b"Hello, world!"
print("Path not provided:")
ApplicationLayer.store_file_bytes(None, data)
print("\nPath provided:")
ApplicationLayer.store_file_bytes(Path("test_files"), data)