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
#include <m3/Log.h>

#include "PEManager.h"
#include "Services.h"
#include "SyscallHandler.h"
#include "RecvBufs.h"

#if defined(__host__)
extern int int_target;
#endif

namespace m3 {

SyscallHandler SyscallHandler::_inst;

#define LOG_SYS(vpe, expr) \
    LOG(KSYSC, (vpe)->name() << "@" << fmt((vpe)->core(), "X") << ": " << expr)

#define SYS_ERROR(vpe, gate, error, msg) { \
        LOG(KERR, (vpe)->name() << ": " << msg << " (" << error << ")"); \
        reply_vmsg((gate), (error)); \
        return; \
    }

struct ReplyInfo {
    explicit ReplyInfo(const DTU::Message &msg)
        : replylbl(msg.replylabel), replyep(msg.reply_epid()), crdep(msg.send_epid()),
          replycrd(msg.length) {
    }

    label_t replylbl;
    int replyep;
    int crdep;
    word_t replycrd;
};

SyscallHandler::SyscallHandler()
        : RequestHandler<SyscallHandler, Syscalls::Operation, Syscalls::COUNT>(),
          _rcvbuf(RecvBuf::create(epid(),
            nextlog2<AVAIL_PES>::val + KVPE::SYSC_CREDIT_ORD, KVPE::SYSC_CREDIT_ORD, 0)),
          _srvrcvbuf(RecvBuf::create(VPE::self().alloc_ep(),
            nextlog2<1024>::val, nextlog2<256>::val, 0)) {
    // configure both receive buffers (we need to do that manually in the kernel)
    KDTU::get().config_recv_local(_rcvbuf.epid(), reinterpret_cast<uintptr_t>(_rcvbuf.addr()),
        _rcvbuf.order(), _rcvbuf.msgorder(), _rcvbuf.flags());
    KDTU::get().config_recv_local(_srvrcvbuf.epid(), reinterpret_cast<uintptr_t>(_srvrcvbuf.addr()),
        _srvrcvbuf.order(), _srvrcvbuf.msgorder(), _srvrcvbuf.flags());

    add_operation(Syscalls::CREATESRV, &SyscallHandler::createsrv);
    add_operation(Syscalls::CREATESESS, &SyscallHandler::createsess);
    add_operation(Syscalls::CREATEGATE, &SyscallHandler::creategate);
    add_operation(Syscalls::CREATEVPE, &SyscallHandler::createvpe);
    add_operation(Syscalls::ATTACHRB, &SyscallHandler::attachrb);
    add_operation(Syscalls::DETACHRB, &SyscallHandler::detachrb);
    add_operation(Syscalls::EXCHANGE, &SyscallHandler::exchange);
    add_operation(Syscalls::VPECTRL, &SyscallHandler::vpectrl);
    add_operation(Syscalls::DELEGATE, &SyscallHandler::delegate);
    add_operation(Syscalls::OBTAIN, &SyscallHandler::obtain);
    add_operation(Syscalls::ACTIVATE, &SyscallHandler::activate);
    add_operation(Syscalls::REQMEM, &SyscallHandler::reqmem);
    add_operation(Syscalls::DERIVEMEM, &SyscallHandler::derivemem);
    add_operation(Syscalls::REVOKE, &SyscallHandler::revoke);
    add_operation(Syscalls::EXIT, &SyscallHandler::exit);
    add_operation(Syscalls::NOOP, &SyscallHandler::noop);
#if defined(__host__)
    add_operation(Syscalls::INIT, &SyscallHandler::init);
#endif
}

void SyscallHandler::createsrv(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_createsrv();
    KVPE *vpe = gate.session<KVPE>();
    String name;
    capsel_t gatesel, srv;
    is >> gatesel >> srv >> name;
    LOG_SYS(vpe, "syscall::createsrv(gate=" << gatesel
        << ", srv=" << srv << ", name=" << name << ")");

    Capability *gatecap = vpe->capabilities().get(gatesel, Capability::MSG);
    if(gatecap == nullptr || name.length() == 0)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap or name");
    if(ServiceList::get().find(name) != nullptr)
        SYS_ERROR(vpe, gate, Errors::EXISTS, "Service does already exist");

    capsel_t kcap = VPE::self().alloc_cap();
    CapTable::kernel_table().obtain(kcap, gatecap);

    int capacity = 1;   // TODO this depends on the credits that the kernel has
    Service *s = ServiceList::get().add(*vpe, srv, name, kcap, capacity);
    vpe->capabilities().set(srv, new ServiceCapability(s));

#if defined(__host__)
    // TODO ugly hack
    if(name == "interrupts")
        int_target = vpe->pid();
#endif

    // maybe there are VPEs that now have all requirements fullfilled
    PEManager::get().start_pending(ServiceList::get());

    reply_vmsg(gate, Errors::NO_ERROR);
}

static void reply_to_vpe(KVPE &vpe, const ReplyInfo &info, const void *msg, size_t size) {
    KDTU::get().reply_to(vpe.core(), info.replyep, info.crdep, info.replycrd, info.replylbl, msg, size);
}

void SyscallHandler::createsess(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_createsess();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t cap;
    String name;
    is >> cap >> name;
    LOG_SYS(vpe, "syscall::createsess(name=" << name << ", cap=" << cap << ")");

    if(!vpe->capabilities().unused(cap))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap");

    Service *s = ServiceList::get().find(name);
    if(!s || s->closing)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Unknown service");

    ReplyInfo rinfo(is.message());
    Reference<Service> rsrv(s);
    vpe->service_gate().subscribe([this, rsrv, cap, vpe, rinfo]
            (RecvGate &sgate, Subscriber<RecvGate&> *sub) {
        EVENT_TRACER_Syscall_createsess();
        GateIStream reply(sgate);
        Errors::Code res;
        reply >> res;
        if(res != Errors::NO_ERROR) {
            LOG(KSYSC, vpe->id() << ": Server denied session creation (" << res << ")");
            auto reply = create_vmsg(res);
            reply_to_vpe(*vpe, rinfo, reply.bytes(), reply.total());
        }
        else {
            word_t sess;
            reply >> sess;

            // inherit the session-cap from the service-cap. this way, it will be automatically
            // revoked if the service-cap is revoked
            Capability *srvcap = rsrv->vpe().capabilities().get(rsrv->selector(), Capability::SERVICE);
            assert(srvcap != nullptr);
            SessionCapability *sesscap = new SessionCapability(const_cast<Service*>(&*rsrv), sess);
            vpe->capabilities().inherit(srvcap, sesscap);
            vpe->capabilities().set(cap, sesscap);

            auto reply = create_vmsg(res);
            reply_to_vpe(*vpe, rinfo, reply.bytes(), reply.total());
        }

        const_cast<Reference<Service>&>(rsrv)->received_reply();
        vpe->service_gate().unsubscribe(sub);
    });

    AutoGateOStream msg(vostreamsize(ostreamsize<server_type::Command>(), is.remaining()));
    msg << server_type::OPEN;
    msg.put(is);
    s->send(&vpe->service_gate(), msg.bytes(), msg.total());
    msg.claim();
}

void SyscallHandler::creategate(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_creategate();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t tcap,dstcap;
    label_t label;
    size_t epid;
    word_t credits;
    is >> tcap >> dstcap >> label >> epid >> credits;
    LOG_SYS(vpe, "syscall::creategate(vpe=" << tcap << ", cap=" << dstcap
        << ", label=" << fmt(label, "#0x", sizeof(label_t) * 2)
        << ", ep=" << epid << ", crd=#" << fmt(credits, "0x") << ")");

#if defined(__gem5__)
    if(credits == SendGate::UNLIMITED)
        PANIC("Unlimited credits are not yet supported on gem5");
#endif

    VPECapability *tcapobj = static_cast<VPECapability*>(vpe->capabilities().get(tcap, Capability::VPE));
    if(tcapobj == nullptr)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "VPE capability is invalid");

    // 0 points to the SEPs and can't be delegated to someone else
    if(epid == 0 || epid >= EP_COUNT || !vpe->capabilities().unused(dstcap))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap or ep");

    vpe->capabilities().set(dstcap, new MsgCapability(label, tcapobj->vpe->core(), epid, credits));
    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::createvpe(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_createvpe();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t tcap, mcap;
    String name, core;
    is >> tcap >> mcap >> name >> core;
    LOG_SYS(vpe, "syscall::createvpe(name=" << name << ", core=" << core
        << ", tcap=" << tcap << ", mcap=" << mcap << ")");

    if(!vpe->capabilities().unused(tcap) || !vpe->capabilities().unused(mcap))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid VPE or memory cap");

    const char *corename = core.c_str()[0] == '\0'
        ? PEManager::get().type(vpe->core() - APP_CORES)
        : core.c_str();
    KVPE *nvpe = PEManager::get().create(std::move(name), corename);
    if(nvpe == nullptr)
        SYS_ERROR(vpe, gate, Errors::NO_FREE_CORE, "No free and suitable core found");

    vpe->capabilities().obtain(tcap, nvpe->capabilities().get(0));
    vpe->capabilities().obtain(mcap, nvpe->capabilities().get(1));

    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::attachrb(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_attachrb();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t tcap;
    uintptr_t addr;
    size_t ep;
    int order, msgorder;
    uint flags;
    is >> tcap >> ep >> addr >> order >> msgorder >> flags;
    LOG_SYS(vpe, "syscall::attachrb(vpe=" << tcap << ", ep=" << ep
        << ", addr=" << fmt(addr, "p") << ", size=" << fmt(1UL << order, "#x")
        << ", msgsize=" << fmt(1UL << msgorder, "#x") << ", flags=" << fmt(flags, "#x") << ")");

    VPECapability *tcapobj = static_cast<VPECapability*>(vpe->capabilities().get(tcap, Capability::VPE));
    if(tcapobj == nullptr)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "VPE capability is invalid");

    Errors::Code res = RecvBufs::attach(tcapobj->vpe->core(), ep, addr, order, msgorder, flags);
    if(res != Errors::NO_ERROR)
        SYS_ERROR(vpe, gate, res, "Unable to attach receive buffer");

    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::detachrb(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_detachrb();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t tcap;
    size_t ep;
    is >> tcap >> ep;
    LOG_SYS(vpe, "syscall::detachrb(vpe=" << tcap << ", ep=" << ep << ")");

    VPECapability *tcapobj = static_cast<VPECapability*>(vpe->capabilities().get(tcap, Capability::VPE));
    if(tcapobj == nullptr)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "VPE capability is invalid");

    RecvBufs::detach(tcapobj->vpe->core(), ep);
    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::exchange(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_exchange();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t tcap;
    CapRngDesc own, other;
    bool obtain;
    is >> tcap >> own >> other >> obtain;
    LOG_SYS(vpe, "syscall::exchange(vpe=" << tcap << ", own=" << own
        << ", other=" << other << ", obtain=" << obtain << ")");

    VPECapability *vpecap = static_cast<VPECapability*>(
            vpe->capabilities().get(tcap, Capability::VPE));
    if(vpecap == nullptr)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid VPE cap");

    KVPE *t1 = obtain ? vpecap->vpe : vpe;
    KVPE *t2 = obtain ? vpe : vpecap->vpe;
    Errors::Code res = do_exchange(t1, t2, own, other, obtain);
    reply_vmsg(gate, res);
}

void SyscallHandler::vpectrl(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_vpectrl();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t tcap;
    Syscalls::VPECtrl op;
    int pid;
    is >> tcap >> op >> pid;
    LOG_SYS(vpe, "syscall::vpectrl(vpe=" << tcap << ", op=" << op
            << ", pid=" << pid << ")");

    VPECapability *vpecap = static_cast<VPECapability*>(
            vpe->capabilities().get(tcap, Capability::VPE));
    if(vpecap == nullptr)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap");

    switch(op) {
        case Syscalls::VCTRL_START:
            vpecap->vpe->start(0, nullptr, pid);
            reply_vmsg(gate, Errors::NO_ERROR);
            break;

        case Syscalls::VCTRL_WAIT:
            if(vpecap->vpe->state() == KVPE::DEAD)
                reply_vmsg(gate, Errors::NO_ERROR, vpecap->vpe->exitcode());
            else {
                ReplyInfo rinfo(is.message());
                vpecap->vpe->subscribe_exit([vpe, is, rinfo] (int exitcode, Subscriber<int> *) {
                    EVENT_TRACER_Syscall_vpectrl();
                    auto reply = create_vmsg(Errors::NO_ERROR,exitcode);
                    reply_to_vpe(*vpe, rinfo, reply.bytes(), reply.total());
                });
            }
            break;
    }
}

void SyscallHandler::reqmem(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_reqmem();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t cap;
    uintptr_t addr;
    size_t size;
    int perms;
    is >> cap >> addr >> size >> perms;
    LOG_SYS(vpe, "syscall::reqmem(cap=" << cap
        << ", addr=#" << fmt(addr, "x") << ", size=#" << fmt(size, "x")
        << ", perms=" << perms << ")");

    if(!vpe->capabilities().unused(cap))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap");
    if(size == 0 || (size & MemGate::RWX) || perms == 0 || (perms & ~(MemGate::RWX)))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Size or permissions invalid");

    MainMemory &mem = MainMemory::get();
    if(addr != (uintptr_t)-1 && Math::overlap(addr, size, mem.addr(), mem.size()))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Addr+size overlap with allocatable memory");

    if(addr == (uintptr_t)-1) {
        addr = mem.map().allocate(size);
        if(addr == (uintptr_t)-1)
            SYS_ERROR(vpe, gate, Errors::OUT_OF_MEM, "Not enough memory");
    }
    else
        addr += mem.base();

    LOG(KSYSC, "Requested " << (size / 1024) << " KiB of memory @ " << fmt(addr, "p"));

    // TODO if addr was 0, we don't want to free it on revoke
    vpe->capabilities().set(cap, new MemCapability(addr, size, perms, MEMORY_CORE, mem.epid()));
    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::derivemem(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_derivemem();
    KVPE *vpe = gate.session<KVPE>();
    capsel_t src, dst;
    size_t offset, size;
    int perms;
    is >> src >> dst >> offset >> size >> perms;
    LOG_SYS(vpe, "syscall::derivemem(src=" << src << ", dst=" << dst
        << ", size=" << size << ", off=" << offset << ", perms=" << perms << ")");

    MemCapability *srccap = static_cast<MemCapability*>(
            vpe->capabilities().get(src, Capability::MEM));
    if(srccap == nullptr || !vpe->capabilities().unused(dst))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap(s)");

    if(offset + size < offset || offset + size > srccap->obj->credits || size == 0 ||
            (perms & ~(MemGate::RWX)))
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid args");

    MemCapability *dercap = static_cast<MemCapability*>(vpe->capabilities().obtain(dst, srccap));
    dercap->obj = Reference<MsgObject>(new MemObject(
        (srccap->obj->label & ~MemGate::RWX) + offset,
        size,
        perms & (srccap->obj->label & MemGate::RWX),
        srccap->obj->core,
        srccap->obj->epid
    ));
    dercap->obj->derived = true;
    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::delegate(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_delegate();
    exchange_over_sess(gate, is, false);
}

void SyscallHandler::obtain(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_obtain();
    exchange_over_sess(gate, is, true);
}

Errors::Code SyscallHandler::do_exchange(KVPE *v1, KVPE *v2, const CapRngDesc &c1, const CapRngDesc &c2, bool obtain) {
    KVPE &src = obtain ? *v2 : *v1;
    KVPE &dst = obtain ? *v1 : *v2;
    const CapRngDesc &srcrng = obtain ? c2 : c1;
    const CapRngDesc &dstrng = obtain ? c1 : c2;

    if((obtain && c2.count() > c1.count()) || (!obtain && c2.count() != c1.count())) {
        LOG(KSYSC, v1->id() << ": Server gave me invalid CRD (" << Errors::INV_ARGS << ")");
        return Errors::INV_ARGS;
    }
    if(!dst.capabilities().range_unused(dstrng)) {
        LOG(KSYSC, v1->id() << ": Invalid destination caps (" << Errors::INV_ARGS << ")");
        return Errors::INV_ARGS;
    }

    for(uint i = 0; i < c2.count(); ++i) {
        capsel_t srccap = srcrng.start() + i;
        capsel_t dstcap = dstrng.start() + i;
        Capability *scapobj = src.capabilities().get(srccap);
        assert(dst.capabilities().get(dstcap) == nullptr);
        dst.capabilities().obtain(dstcap, scapobj);
    }
    return Errors::NO_ERROR;
}

void SyscallHandler::exchange_over_sess(RecvGate &gate, GateIStream &is, bool obtain) {
    KVPE *vpe = gate.session<KVPE>();
    capsel_t sesscap;
    CapRngDesc caps;
    is >> sesscap >> caps;
    // TODO compiler-bug? if I try to print caps, it hangs on T2!? doing it here manually works
    LOG_SYS(vpe, "syscall::" << (obtain ? "obtain" : "delegate")
            << "(sess=" << sesscap << ", caps=" << caps.start() << ":" << caps.count() << ")");

    SessionCapability *sess = static_cast<SessionCapability*>(
        vpe->capabilities().get(sesscap, Capability::SESSION));
    if(sess == nullptr)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid session-cap");
    if(sess->obj->srv->closing)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Server is shutting down");

    ReplyInfo rinfo(is.message());
    // only pass in the service-reference. we can't be sure that the session will still exist
    // when we receive the reply
    Reference<Service> rsrv(sess->obj->srv);
    vpe->service_gate().subscribe([this, rsrv, caps, vpe, obtain, rinfo]
            (RecvGate &sgate, Subscriber<RecvGate&> *sub) {
        EVENT_TRACER_Syscall_delob_done();
        CapRngDesc srvcaps;

        GateIStream reply(sgate);
        Errors::Code res;
        reply >> res;
        if(res != Errors::NO_ERROR) {
            LOG(KSYSC, vpe->id() << ": Server denied cap-transfer (" << res << ")");

            auto reply = create_vmsg(res);
            reply_to_vpe(*vpe, rinfo, reply.bytes(), reply.total());
            goto error;
        }

        reply >> srvcaps;
        if((res = do_exchange(vpe, &rsrv->vpe(), caps, srvcaps, obtain)) != Errors::NO_ERROR) {
            auto reply = create_vmsg(res);
            reply_to_vpe(*vpe, rinfo, reply.bytes(), reply.total());
            goto error;
        }

        {
            AutoGateOStream msg(vostreamsize(ostreamsize<Errors, CapRngDesc>(), reply.remaining()));
            msg << Errors::NO_ERROR;
            msg.put(reply);
            reply_to_vpe(*vpe, rinfo, msg.bytes(), msg.total());
        }

    error:
        const_cast<Reference<Service>&>(rsrv)->received_reply();
        vpe->service_gate().unsubscribe(sub);
    });

    AutoGateOStream msg(vostreamsize(ostreamsize<server_type::Command, word_t, CapRngDesc>(), is.remaining()));
    msg << (obtain ? server_type::OBTAIN : server_type::DELEGATE) << sess->obj->ident << caps.count();
    msg.put(is);
    sess->obj->srv->send(&vpe->service_gate(), msg.bytes(), msg.total());
    msg.claim();
}

static Errors::Code do_activate(KVPE *vpe, size_t epid, MsgCapability *oldcapobj, MsgCapability *newcapobj) {
    if(newcapobj) {
        LOG_SYS(vpe, "Setting ep[" << epid << "] to lbl="
                << fmt(newcapobj->obj->label, "#0x", sizeof(label_t) * 2) << ", core=" << newcapobj->obj->core
                << ", ep=" << newcapobj->obj->epid
                << ", crd=#" << fmt(newcapobj->obj->credits, "x"));
    }
    else
        LOG_SYS(vpe, "Setting ep[" << epid << "] to NUL");

    Errors::Code res = vpe->xchg_ep(epid, oldcapobj, newcapobj);
    if(res != Errors::NO_ERROR)
        return res;

    if(oldcapobj)
        oldcapobj->localepid = -1;
    if(newcapobj)
        newcapobj->localepid = epid;
    return Errors::NO_ERROR;
}

void SyscallHandler::activate(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_activate();
    KVPE *vpe = gate.session<KVPE>();
    size_t epid;
    capsel_t oldcap, newcap;
    is >> epid >> oldcap >> newcap;
    LOG_SYS(vpe, "syscall::activate(ep=" << epid << ", old=" <<
        oldcap << ", new=" << newcap << ")");

    MsgCapability *oldcapobj = oldcap == Cap::INVALID ? nullptr : static_cast<MsgCapability*>(
            vpe->capabilities().get(oldcap, Capability::MSG | Capability::MEM));
    MsgCapability *newcapobj = newcap == Cap::INVALID ? nullptr : static_cast<MsgCapability*>(
            vpe->capabilities().get(newcap, Capability::MSG | Capability::MEM));
    // ep 0 can never be used for sending
    if(epid == 0 || (oldcapobj == nullptr && newcapobj == nullptr)) {
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Invalid cap(s) (old=" << oldcap << "," << oldcapobj
            << ", new=" << newcap << "," << newcapobj << ")");
    }

    if(newcapobj && newcapobj->type == Capability::MSG) {
        if(!RecvBufs::is_attached(newcapobj->obj->core, newcapobj->obj->epid)) {
            ReplyInfo rinfo(is.message());
            LOG_SYS(vpe, "waiting for receive buffer "
                << newcapobj->obj->core << ":" << newcapobj->obj->epid << " to get attached");

            auto callback = [rinfo, vpe, epid, oldcapobj, newcapobj](bool success, Subscriber<bool> *) {
                EVENT_TRACER_Syscall_activate();
                Errors::Code res = success ? Errors::NO_ERROR : Errors::GONE;
                if(success)
                    res = do_activate(vpe, epid, oldcapobj, newcapobj);
                if(res != Errors::NO_ERROR)
                    LOG(KERR, vpe->name() << ": activate failed (" << res << ")");

                auto reply = create_vmsg(res);
                reply_to_vpe(*vpe, rinfo, reply.bytes(), reply.total());
            };
            RecvBufs::subscribe(newcapobj->obj->core, newcapobj->obj->epid, callback);
            return;
        }
    }

    Errors::Code res = do_activate(vpe, epid, oldcapobj, newcapobj);
    if(res != Errors::NO_ERROR)
        SYS_ERROR(vpe, gate, res, "cmpxchg failed");
    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::revoke(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_revoke();
    KVPE *vpe = gate.session<KVPE>();
    CapRngDesc crd;
    is >> crd;
    LOG_SYS(vpe, "syscall::revoke(" << crd.start() << ":" << crd.count() << ")");

    if(crd.start() < 2)
        SYS_ERROR(vpe, gate, Errors::INV_ARGS, "Cap 0 and 1 are not revokeable");

    Errors::Code res = vpe->capabilities().revoke(crd);
    if(res != Errors::NO_ERROR)
        SYS_ERROR(vpe, gate, res, "Revoke failed");

    reply_vmsg(gate, Errors::NO_ERROR);
}

void SyscallHandler::exit(RecvGate &gate, GateIStream &is) {
    EVENT_TRACER_Syscall_exit();
    KVPE *vpe = gate.session<KVPE>();
    int exitcode;
    is >> exitcode;
    LOG_SYS(vpe, "syscall::exit(" << exitcode << ")");

    vpe->exit(exitcode);
    vpe->unref();

    // if there are no VPEs left, we can stop everything
    if(PEManager::get().used() == 0) {
        PEManager::destroy();
        // ensure that the workloop stops
        _rcvbuf.detach();
        _srvrcvbuf.detach();
    }
    // if there are only daemons left, start the shutdown-procedure
    else if(PEManager::get().used() == PEManager::get().daemons())
        PEManager::shutdown();
}

void SyscallHandler::noop(RecvGate &gate, GateIStream &) {
    reply_vmsg(gate, 0);
}

#if defined(__host__)
void SyscallHandler::init(RecvGate &gate,GateIStream &is) {
    KVPE *vpe = gate.session<KVPE>();
    void *addr;
    is >> addr;
    vpe->activate_sysc_ep(addr);
    LOG_SYS(vpe, "syscall::init(" << addr << ")");

    // switch to this endpoint to ensure that we don't have old values programmed in our DMAUnit.
    // actually, this is currently only necessary for exec, i.e. where the address changes for
    // the same VPE.
    EPMux::get().switch_to(&vpe->seps_gate());
    reply_vmsg(gate, Errors::NO_ERROR);
}
#endif

}
