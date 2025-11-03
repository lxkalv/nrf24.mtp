import struct
import time
import sys
import zlib
import math

# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
payload_size  = 22
pages_number  = 10 

# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def INFO(message: str) -> None:
    """
    Prints a message to the console with the blue prefix `[INFO]:`
    """
    print(f"\033[34m[INFO]:\033[0m {message}")



def SUCC(message: str) -> None:
    """
    Prints a message to the console with the green prefix `[SUCC]:`
    """
    print(f"\033[32m[SUCC]:\033[0m {message}")


def ERROR(message: str) -> None:
    """
    Prints a message to the console with the red prefix `[~ERR]:`
    """
    print(f"\033[31m[~ERR]:\033[0m {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
#Activate compressor
compresor = zlib.compressobj(level=6)

# open the file to read
with open("Test_compresion/original_files/quijote.txt", "rb") as file:
 content = file.read()
 content_len = len(content)
 file_compressed = compresor.compress(content)
 compresion_ratio_file = len(file_compressed)/content_len
 print(f"File size (bytes): {content_len}")
 print(f"File size compressed (bytes): {len(file_compressed)}")
 print(f"Compresion ratio file: {compresion_ratio_file}")
 #Separate into pages
pages = []
page_size = math.ceil(content_len / pages_number)
compressed_bytes = 0
tic     = time.monotonic()
for idx in range(0, content_len, page_size):
    page = content[idx : idx + page_size]
    print(f"Page size without compresion (bytes): {len(page)}")
    page_compressed = compresor.compress(page)
    page_compressed += compresor.flush(zlib.Z_SYNC_FLUSH)
    pages.append(page_compressed)
    print(f"Page size with compresion (bytes): {len(page_compressed)}")
    compressed_bytes += len(page_compressed)
tac     = time.monotonic()
INFO(f"Tiempo tardado {tac-tic}")
compresion_ratio = compressed_bytes/content_len
print(f"File size (bytes): {content_len}")
print(f"File after compression (bytes): {compressed_bytes}")
print(f"Compresion ratio: {compresion_ratio}")



