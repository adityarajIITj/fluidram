"""
Plain Linux Memory Management Subsystem Simulator.

Models standard Linux virtual memory management:
- 4KB page frames
- Standard zram (LZO / LZ4 / Deflate dictionary compression)
- LRU active/inactive page lists (vmscan.c)
- Disk swap file (with realistic SSD/NVMe I/O latency)
- Linux Out-Of-Memory (OOM) Killer (oom_kill.c)
"""

import time
import zlib
import collections
from typing import Dict, List, Tuple, Optional

PAGE_SIZE = 4096

class PlainLinuxZram:
    """Standard Linux zram block device simulator."""
    def __init__(self, disksize_bytes: int, comp_algorithm: str = "lzo"):
        self.disksize_bytes = disksize_bytes
        self.comp_algorithm = comp_algorithm
        self.num_pages = disksize_bytes // PAGE_SIZE
        self.pages: Dict[int, bytes] = {}
        self.mem_used_total = 0
        self.orig_data_size = 0

    def write_page(self, pgoff: int, page_bytes: bytes) -> bool:
        if pgoff >= self.num_pages:
            return False
        # Standard dictionary compression (zlib level 1 mimics fast lzo/lz4 ratio & speed)
        comp = zlib.compress(page_bytes, level=1)
        old_size = len(self.pages.get(pgoff, b""))
        self.pages[pgoff] = comp
        self.mem_used_total += len(comp) - old_size
        self.orig_data_size += PAGE_SIZE
        return True

    def read_page(self, pgoff: int) -> bytes:
        if pgoff not in self.pages:
            return bytes(PAGE_SIZE)
        return zlib.decompress(self.pages[pgoff])

    def discard_page(self, pgoff: int):
        if pgoff in self.pages:
            self.mem_used_total -= len(self.pages[pgoff])
            del self.pages[pgoff]


class PlainLinuxMemorySubsystem:
    """
    Full Plain Linux Memory Subsystem Simulator.
    Simulates physical RAM, LRU reclamation, disk swap, and the OOM killer.
    """
    def __init__(self, physical_ram_bytes: int, swap_disk_bytes: int, enable_zram: bool = True):
        self.physical_ram_bytes = physical_ram_bytes
        self.swap_disk_bytes = swap_disk_bytes
        self.enable_zram = enable_zram
        
        self.max_ram_pages = physical_ram_bytes // PAGE_SIZE
        self.ram_pages: Dict[int, bytes] = {} # pgoff -> raw 4KB bytes
        self.lru_list: collections.OrderedDict = collections.OrderedDict() # pgoff -> timestamp
        
        self.zram = PlainLinuxZram(physical_ram_bytes // 2) if enable_zram else None
        self.disk_swap_pages: Dict[int, bytes] = {} # pgoff -> raw 4KB bytes on disk
        
        # Telemetry metrics
        self.major_page_faults = 0
        self.minor_page_faults = 0
        self.disk_io_wait_ms = 0.0
        self.oom_kills = 0
        self.active_processes: Dict[int, List[int]] = {} # pid -> list of pgoffs

    def allocate_page(self, pid: int, pgoff: int, data: bytes) -> bool:
        """Allocates a virtual page into memory, handling swapout and OOM if saturated."""
        if len(data) != PAGE_SIZE:
            data = data.ljust(PAGE_SIZE, b"\x00")[:PAGE_SIZE]
            
        if pid not in self.active_processes:
            self.active_processes[pid] = []
            
        # Check if physical RAM has capacity
        if len(self.ram_pages) < self.max_ram_pages:
            self.ram_pages[pgoff] = data
            self.lru_list[pgoff] = time.time()
            self.active_processes[pid].append(pgoff)
            self.minor_page_faults += 1
            return True

        # Physical RAM is full: perform page reclamation via LRU (vmscan.c)
        evict_pgoff, _ = self.lru_list.popitem(last=False)
        evict_data = self.ram_pages.pop(evict_pgoff)
        
        swapped = False
        # Try zram first if enabled
        if self.zram and self.zram.mem_used_total + 1024 < (self.physical_ram_bytes // 3):
            self.zram.write_page(evict_pgoff, evict_data)
            swapped = True
        elif len(self.disk_swap_pages) * PAGE_SIZE < self.swap_disk_bytes:
            # Swap to disk: disk I/O cost (simulated 1.5ms per write)
            self.disk_swap_pages[evict_pgoff] = evict_data
            self.disk_io_wait_ms += 1.5
            swapped = True

        if not swapped:
            # Memory exhausted on RAM, zram, and disk swap: Trigger OOM Killer!
            self.oom_kills += 1
            # Terminate the highest memory consumer
            victim_pid = max(self.active_processes.keys(), key=lambda p: len(self.active_processes[p]))
            for page in self.active_processes[victim_pid]:
                self.ram_pages.pop(page, None)
                self.disk_swap_pages.pop(page, None)
                self.lru_list.pop(page, None)
            del self.active_processes[victim_pid]
            return False

        # Store incoming page into newly reclaimed RAM slot
        self.ram_pages[pgoff] = data
        self.lru_list[pgoff] = time.time()
        self.active_processes[pid].append(pgoff)
        self.minor_page_faults += 1
        return True

    def read_page(self, pid: int, pgoff: int) -> bytes:
        """Reads a virtual page. If page is swapped out, triggers major page fault."""
        if pgoff in self.ram_pages:
            # Hit in physical RAM
            self.lru_list.move_to_end(pgoff)
            return self.ram_pages[pgoff]

        # Major page fault: Page is on disk swap!
        if pgoff in self.disk_swap_pages:
            self.major_page_faults += 1
            # NVMe/SSD read penalty: 2.5ms latency
            self.disk_io_wait_ms += 2.5
            data = self.disk_swap_pages.pop(pgoff)
            # Reclaim RAM for faulted page
            self.allocate_page(pid, pgoff, data)
            return data

        if self.zram and pgoff in self.zram.pages:
            self.minor_page_faults += 1
            data = self.zram.read_page(pgoff)
            self.allocate_page(pid, pgoff, data)
            return data

        return bytes(PAGE_SIZE)

    def get_physical_resident_mb(self) -> float:
        ram_bytes = len(self.ram_pages) * PAGE_SIZE
        zram_bytes = self.zram.mem_used_total if self.zram else 0
        return (ram_bytes + zram_bytes) / (1024 * 1024)

    def get_effective_density(self, virtual_requested_bytes: int) -> float:
        phys_bytes = (len(self.ram_pages) * PAGE_SIZE) + (self.zram.mem_used_total if self.zram else 0)
        if phys_bytes == 0:
            return 1.0
        return virtual_requested_bytes / phys_bytes
