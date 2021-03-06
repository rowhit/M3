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

#include <m3/cap/Cap.h>
#include <m3/cap/VPE.h>
#include <m3/Syscalls.h>

namespace m3 {

void Cap::release() {
    if(_sel != INVALID) {
        if(~_flags & KEEP_SEL)
            VPE::self().free_cap(sel());
        if(~_flags & KEEP_CAP)
            Syscalls::get().revoke(CapRngDesc(sel()));
    }
}

}
