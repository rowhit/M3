/*
 * Copyright (C) 2015, Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of M3 (Microkernel-based SysteM for Heterogeneous Manycores).
 *
 * M3 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * M3 is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#include <m3/tracing/Tracing.h>
#include <m3/Config.h>
#include <iostream>

using namespace std;

int main() {
#if !defined(__host__)
    cout << "[memlayout]\n";
    cout << "ARGC_ADDR = "          << ARGC_ADDR << "\n";
    cout << "ARGV_ADDR = "          << ARGV_ADDR << "\n";
    cout << "ARGV_SIZE = "          << ARGV_SIZE << "\n";
    cout << "ARGV_START = "         << ARGV_START << "\n";
    cout << "BOOT_ENTRY = "         << BOOT_ENTRY << "\n";
    cout << "BOOT_SP = "            << BOOT_SP << "\n";
    cout << "BOOT_LAMBDA = "        << BOOT_LAMBDA << "\n";
    cout << "BOOT_EPS = "           << BOOT_EPS << "\n";
    cout << "BOOT_CAPS = "          << BOOT_CAPS << "\n";
    cout << "BOOT_MOUNTS = "        << BOOT_MOUNTS << "\n";
    cout << "STATE_SPACE = "        << STATE_SPACE << "\n";
    cout << "BOOT_EXIT = "          << BOOT_EXIT << "\n";
    cout << "CORE_CONF_SIZE = "     << sizeof(m3::CoreConf) << "\n";
    cout << "CORE_CONF = "          << CONF_LOCAL << "\n";
#endif

#if defined(__t2__) || defined(__t3__)
    cout << "SERIAL_ACK = "         << SERIAL_ACK << "\n";
    cout << "SERIAL_INWAIT = "      << SERIAL_INWAIT << "\n";
    cout << "SERIAL_BUFSIZE = "     << SERIAL_BUFSIZE << "\n";
    cout << "SERIAL_BUF = "         << SERIAL_BUF << "\n";
#endif

#if defined(__t2__)
    cout << "BOOT_DATA = "          << BOOT_DATA << "\n";
    cout << "DRAM_CCOUNT = "        << DRAM_CCOUNT << "\n";
    cout << "CCOUNT_CM = "          << CCOUNT_CM << "\n";
    cout << "TRACE_MEMBUF_SIZE = "  << TRACE_MEMBUF_SIZE << "\n";
    cout << "TRACE_MEMBUF_ADDR = "  << TRACE_MEMBUF_ADDR << "\n";
    cout << "DRAM_FILE_AREA = "     << DRAM_FILE_AREA << "\n";
    cout << "DRAM_BLOCKNO = "       << DRAM_BLOCKNO << "\n";
    cout << "DRAM_FILESIZE = "      << DRAM_FILESIZE << "\n";
    cout << "DRAM_FILENAME = "      << DRAM_FILENAME << "\n";
    cout << "DRAM_FILENAME_LEN = "  << DRAM_FILENAME_LEN << "\n";
    cout << "FS_IMG_OFFSET = "      << FS_IMG_OFFSET << "\n";
#endif
    return 0;
}
