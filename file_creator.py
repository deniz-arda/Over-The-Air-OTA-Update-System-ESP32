#!/usr/bin/env python3
"""
Create a 1KB test binary file for OTA testing
"""

def create_test_binary(filename, size_kb=1):
    """Create a test binary file with pattern data"""
    size_bytes = size_kb * 1024
    
    # Create a repeating pattern
    data = bytearray()
    for i in range(size_bytes):
        # Create a simple pattern: incrementing bytes with some markers
        if i % 256 == 0:
            data.append(0xAA)  # Marker every 256 bytes
        elif i % 128 == 0:
            data.append(0x55)  # Marker every 128 bytes  
        else:
            data.append(i % 256)  # Incrementing pattern
    
    # Write to file
    with open(filename, 'wb') as f:
        f.write(data)
    
    print(f"Created {filename}: {len(data)} bytes")
    
    # Verify file size
    import os
    actual_size = os.path.getsize(filename)
    print(f"Actual file size: {actual_size} bytes ({actual_size/1024:.1f} KB)")

if __name__ == "__main__":
    # Create different test files
    create_test_binary("test_1kb.bin", 1)
    create_test_binary("test_512b.bin", 0.5)  # 512 bytes
    create_test_binary("test_2kb.bin", 2)
    
    print("\nTest files created successfully!")
    print("Try: python ota_host.py COM5 --file test_1kb.bin --version 1")