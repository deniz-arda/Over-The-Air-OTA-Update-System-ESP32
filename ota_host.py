import serial
import time
import struct
import os
from typing import Optional, Tuple
from dataclasses import dataclass
from enum import Enum

class Command(Enum):
    #Command opcodes for host-ESP32 communication
    INIT_UPDATE = 0x01
    SEND_PACKET = 0x02
    FINISH_UPDATE = 0x03
    GET_VERSION = 0x04
    ABORT_UPDATE = 0x05

class Response(Enum):
    #Response codes from ESP32
    ACK = 0x10
    NACK = 0x11
    READY = 0x12
    CHECKSUM_ERROR = 0x13
    VERSION_INFO = 0x14
    ERROR = 0x15

@dataclass
class VersionInfo:
    #Version information structure
    current_version: int
    backup_version: int
    current_address: int
    backup_address: int

class OTAHost:
    # Fixed Host program for OTA updates to ESP32
    
    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.serial = None
        self.packet_size = 16
        self.timeout = 5.0
        self.max_retries = 3
        
        # Memory layout configuration
        self.version_1_address = 0x200000
        self.version_2_address = 0x300000
        
    def connect(self) -> bool:
        # Establishing serial connection to ESP32
        try:
            self.serial = serial.Serial(
                self.port, 
                self.baudrate, 
                timeout=self.timeout,
                write_timeout=self.timeout
            )
            
            # Clear buffers and wait for ESP32 startup
            time.sleep(2)
            self.serial.reset_input_buffer()
            self.serial.reset_output_buffer()
            
            print(f"Connected to ESP32 on {self.port}")
            return True
        except Exception as e:
            print(f"Connection failed: {e}")
            return False
            
    def disconnect(self):
        # Close serial connection
        if self.serial and self.serial.is_open:
            self.serial.close()
            print("Disconnected from ESP32")
    
    def send_command(self, cmd: Command, data: bytes = b'') -> bool:
        # Send command to ESP32
        if not self.serial or not self.serial.is_open:
            print("Serial connection not open")
            return False
            
        try:
            # Command format: [CMD][LENGTH][DATA][XOR_CHECKSUM]
            length = len(data)
            packet = struct.pack('BB', cmd.value, length) + data
            
            # Calculate XOR checksum
            checksum = 0
            for byte in packet:
                checksum ^= byte
            
            packet += struct.pack('B', checksum)
            
            # Debug output
            print(f"Sending {cmd.name}: {' '.join(f'{b:02X}' for b in packet[:10])}{'...' if len(packet) > 10 else ''}")
            
            self.serial.write(packet)
            self.serial.flush()
            return True
            
        except Exception as e:
            print(f"Failed to send command: {e}")
            return False
    
    def read_response(self) -> Tuple[Optional[Response], bytes]:
        # Read response from ESP32
        if not self.serial or not self.serial.is_open:
            return None, b''
            
        try:
            # Read response header: [RESPONSE][LENGTH]
            header = self.serial.read(2)
            if len(header) != 2:
                print("Failed to read response header")
                return None, b''
                
            response_code, length = struct.unpack('BB', header)
            print(f"Received response: 0x{response_code:02X}, length: {length}")
            
            # Read data and checksum
            data = self.serial.read(length) if length > 0 else b''
            checksum_byte = self.serial.read(1)
            
            if len(checksum_byte) != 1:
                print("Failed to read checksum")
                return None, b''
                
            # Verify checksum
            expected_checksum = response_code ^ length
            for byte in data:
                expected_checksum ^= byte
                
            if expected_checksum != checksum_byte[0]:
                print(f"Response checksum error: expected {expected_checksum:02X}, got {checksum_byte[0]:02X}")
                return None, b''
                
            try:
                response = Response(response_code)
                return response, data
            except ValueError:
                print(f"Unknown response code: 0x{response_code:02X}")
                return None, b''
                
        except Exception as e:
            print(f"Failed to read response: {e}")
            return None, b''
    
    def get_version_info(self) -> Optional[VersionInfo]:
        # Get current version information from ESP32
        if not self.send_command(Command.GET_VERSION):
            return None
            
        response, data = self.read_response()
        if response != Response.VERSION_INFO or len(data) != 16:
            print(f"Failed to get version info: response={response}, data_len={len(data) if data else 0}")
            return None
            
        # Unpack version info: current_ver, backup_ver, current_addr, backup_addr
        current_ver, backup_ver, current_addr, backup_addr = struct.unpack('<IIII', data)
        
        return VersionInfo(
            current_version=current_ver,
            backup_version=backup_ver,
            current_address=current_addr,
            backup_address=backup_addr
        )
    
    def send_update_file(self, filepath: str, new_version: int) -> bool:
        # Send complete update file to ESP32
        if not os.path.exists(filepath):
            print(f"File not found: {filepath}")
            return False
            
        # Get current version info
        version_info = self.get_version_info()
        if not version_info:
            print("Failed to get version information")
            return False
            
        print(f"Current version: {version_info.current_version}")
        print(f"Backup version: {version_info.backup_version}")
        
        # Determine target address
        if version_info.current_version == 0:
            target_address = self.version_1_address
        else:
            if version_info.current_address == self.version_1_address:
                target_address = self.version_2_address
            else:
                target_address = self.version_1_address
        
        print(f"Installing version {new_version} to address 0x{target_address:08X}")
        
        # Read file
        try:
            with open(filepath, 'rb') as f:
                file_data = f.read()
        except Exception as e:
            print(f"Failed to read file: {e}")
            return False
            
        file_size = len(file_data)
        print(f"File size: {file_size} bytes")
        
        # Initialize update
        init_data = struct.pack('<III', new_version, target_address, file_size)
        if not self.send_command(Command.INIT_UPDATE, init_data):
            print("Failed to send init command")
            return False
        
        # Wait for READY response
        print("Waiting for ESP32 to be ready...")
        for attempt in range(self.max_retries):
            response, data = self.read_response()
            if response == Response.READY:
                print("ESP32 ready for update")
                break
            elif response == Response.ERROR:
                if len(data) >= 1:
                    error_messages = {
                        1: "Bad length",
                        2: "Update already in progress", 
                        3: "Invalid file size",
                        4: "Invalid target address",
                        5: "Flash erase failed"
                    }
                    err_code = data[0]
                    print(f"ESP32 error: {error_messages.get(err_code, f'Unknown error {err_code}')}")
                else:
                    print("ESP32 returned generic error")
                return False
            elif response == Response.NACK:
                print("ESP32 rejected init update")
                return False
            else:
                print(f"Unexpected response: {response}, retrying... ({attempt + 1}/{self.max_retries})")
                time.sleep(1)
        else:
            print("ESP32 did not respond READY after retries")
            return False

        # Send file in packets
        total_packets = (file_size + self.packet_size - 1) // self.packet_size
        print(f"Sending {total_packets} packets...")
        
        for packet_num in range(total_packets):
            offset = packet_num * self.packet_size
            end_offset = min(offset + self.packet_size, file_size)
            packet_data = file_data[offset:end_offset]
            
            # Pad packet to 16 bytes if necessary
            if len(packet_data) < self.packet_size:
                packet_data += b'\xFF' * (self.packet_size - len(packet_data))
            
            # Calculate checksum for packet data only
            checksum = 0
            for byte in packet_data:
                checksum ^= byte
            
            # Create packet: seq_num(2) + checksum(1) + data(16) = 19 bytes total
            packet_content = struct.pack('<HB', packet_num, checksum) + packet_data
            
            success = False
            for retry in range(self.max_retries):
                if not self.send_command(Command.SEND_PACKET, packet_content):
                    print(f"Failed to send packet {packet_num}")
                    continue
                    
                response, _ = self.read_response()
                if response == Response.ACK:
                    success = True
                    break
                elif response == Response.CHECKSUM_ERROR:
                    print(f"Checksum error for packet {packet_num}, retrying...")
                    continue
                elif response == Response.NACK:
                    print(f"ESP32 rejected packet {packet_num}")
                    break
                else:
                    print(f"Unexpected response for packet {packet_num}: {response}")
                    
            if not success:
                print(f"Failed to send packet {packet_num} after retries")
                self.send_command(Command.ABORT_UPDATE)
                return False
                
            # Progress indicator
            if packet_num % 50 == 0 or packet_num == total_packets - 1:
                progress = (packet_num + 1) / total_packets * 100
                print(f"Progress: {progress:.1f}% ({packet_num + 1}/{total_packets})")
        
        print("Data transfer complete. Finalizing update...")
        
        # Finish update
        if not self.send_command(Command.FINISH_UPDATE):
            print("Failed to send finish command")
            return False
            
        response, _ = self.read_response()
        if response != Response.ACK:
            print(f"Update finalization failed. Response: {response}")
            return False
            
        print("Update completed successfully!")
        return True

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Fixed OTA Update Host Program')
    parser.add_argument('port', help='Serial port (e.g., COM3 or /dev/ttyUSB0)')
    parser.add_argument('--baudrate', '-b', type=int, default=115200, help='Baud rate')
    parser.add_argument('--file', '-f', help='Binary file to upload')
    parser.add_argument('--version', '-v', type=int, help='Version number for the update')
    parser.add_argument('--info', '-i', action='store_true', help='Get version info only')
    
    args = parser.parse_args()
    
    host = OTAHost(args.port, args.baudrate)
    
    if not host.connect():
        return 1
        
    try:
        if args.info:
            version_info = host.get_version_info()
            if version_info:
                print(f"Current Version: {version_info.current_version}")
                print(f"Backup Version: {version_info.backup_version}")
                print(f"Current Address: 0x{version_info.current_address:08X}")
                print(f"Backup Address: 0x{version_info.backup_address:08X}")
            else:
                print("Failed to get version information")
                return 1
                
        elif args.file and args.version:
            if host.send_update_file(args.file, args.version):
                print("Update successful!")
                time.sleep(1)
                version_info = host.get_version_info()
                if version_info:
                    print(f"New current version: {version_info.current_version}")
            else:
                print("Update failed!")
                return 1
        else:
            print("Use --info to get version info, or --file and --version to perform update")
            
    finally:
        host.disconnect()
    
    return 0

if __name__ == '__main__':
    exit(main())