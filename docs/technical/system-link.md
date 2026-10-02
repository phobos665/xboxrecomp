# System Link: the network card

System Link is plain Ethernet between consoles, and the network stack that
speaks it (XNet) is linked into every title and drives the card's registers
itself. So the card is where this runtime steps in: below it a frame is a
byte array, above it is the title's own lifted code. The proposal and the
plan, including XLink Kai and Insignia, are in
`docs/System Link over the internet.md`.

This page records what exists and what was measured. TimeSplitters 2 is the
reference title.

## What exists

| Piece | Where | Switch |
| --- | --- | --- |
| A unique Ethernet address per save folder | `src/kernel/xbox_nic.c`, answered through `ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR)` in `kernel_xbox.c` | `RECOMP_SYSLINK_MAC=00:50:F2:12:34:56` overrides it |
| A register log of the card at 0xFEF00000 | `src/kernel/xbox_nic.c`, routed from `route_device_fault` in the title's `main.c` | `RECOMP_NIC_TRACE=1`, `RECOMP_NIC_TRACE_BUDGET=<lines>` |

There is no card model yet. Without `RECOMP_NIC_TRACE` the register page is
the plain memory it always was, and with it the page traps but every access
is answered from a copy of what the guest wrote, so the title sees the same
plain memory either way. A title on a dead network shows exactly what it
should: TimeSplitters 2 searches, then says "No System Link Games Found".

### The Ethernet address

Every install used to be 00:50:F2:00:00:01, and two consoles with one
address cannot share a System Link game. Now the first time a title asks, an
address is chosen at random under Microsoft's 00:50:F2 prefix and written to
`console.ini` in the save folder, beside the partition images:

```
[NET] Ethernet address 00:50:F2:8E:C2:08 (new, saved to console.ini)
```

The save folder because that is the console's disk. On a console the
identity (the EEPROM) and the disk go together, and Insignia will need them
to match. It also means a scripted run under `RECOMP_SAVE_DIR` gets an
identity of its own and never writes the player's.

A title asks only when its network code starts, not at boot. TimeSplitters 2
asks when System Link's Join Game or the hosting path starts XNet.

### The register log

```
[NIC] write +0x0A8 MacAddrA           = 0x8EF25000 (4) sub_0020FD1E+0x131D  [first]
[NIC] write +0x144 TxRxControl        = 0x00000001 (4) sub_0020FACA+0x138
[NIC]   ... and 3411 more times
```

A run of identical accesses prints once with a count. Past the line budget,
only the first access to each register still prints. Every five seconds the
timer thread prints a summary of each register touched, and for the first
three summaries after XNet sets them up, the descriptor rings and the start
of each queued frame. An instruction the decoder does not know stops the log
and leaves the page as plain memory, rather than stopping the title.

The register names are Linux forcedeth's for the same nForce MAC, there as a
reading aid.

## Getting TimeSplitters 2 to System Link in a scripted run

```
RECOMP_INPUT_SEQ=12000:start,16000:start,20000:start,24000:start,46000:a,56000:down:100,60000:a,66000:down:100,68000:down:100,71000:a,76000:down:100,79000:a
```

That is: the start screens, profile storage (the "saving profile" screen
moves on by itself, so no second A), Arcade, System Link, Join Game. Two
things that cost runs. A `down` held for 300 ms auto-repeats and moves the
cursor two rows, so hold it 100 ms. And the script's clock starts at the
title's first pad read, not at process start.

TimeSplitters 2 writes its profiles through `\Device\Harddisk0\Partition1\`,
which maps to the game folder, so `RECOMP_SAVE_DIR` does not isolate them.
Move `games/Time Splitters 2/UDATA/4553000a/<12 hex digits>/` aside before
scripted runs and put them back after.

### A discovery gap on the way

Selecting System Link made an indirect call to 0x0009D530, which the
disassembler had never found. The skipped call returned 0, the title went
into its error path (`[INT3]` after a call it treats as never returning),
and it crashed writing through 0xFFCFFFF0. Seeded in
`config/seeds/4553000A.json` with `tools.seed_from_log`, re-run from the
disasm stage, and the menu opens. (`seed_from_log` rewrote the seed file with
CRLF line endings; it was put back to LF.)

## What XNet does, measured (TimeSplitters 2, XDK 4721)

From `RECOMP_NIC_TRACE=1` through Join Game. Nothing touches the card before
that point. The driver is five functions at 0x0020F578 to 0x0020FD1E, and its
interrupt routine (0x0020F742) connects on **vector 0**, not the IRQ 4 that
xemu's PCI model uses.

**Reset.** `LinkSpeed`, `ReceiverControl`, `TransmitterControl` cleared, then
`ReceiverStatus` and `TransmitterStatus` read for bit 0 (busy, clear on plain
memory, so fine). Then `TxRxControl = 4` (reset) and a wait of up to 500
reads for **bit 3 of `TxRxControl`**, forcedeth's IDLE. Plain memory never
sets it, so today the wait times out. **The model has to report IDLE.**

**Set-up.** In order: `MIIMask`, `IrqMask = 0`, `+0x200`, `UnknownSetupReg6`,
`TransmitPoll`, `LinkSpeed`; status registers read and written back
(write-1-to-clear acknowledgements); then:

| Register | Value | Meaning |
| --- | --- | --- |
| `MacAddrA` / `MacAddrB` | `0x8EF25000` / `0x000008C2` | 00:50:F2:8E:C2:08, the address `console.ini` chose |
| `MulticastAddrA/B`, `MulticastMaskA/B` | all ones | receive broadcasts |
| `OffloadConfig` | `0x5EE` | 1518, the largest frame |
| `PacketFilterFlags` | `0x007F0020` | |
| `Misc1` | `0x003B0F3E` | |
| `TxRingPhysAddr` | `0x03684000` | send descriptors |
| `RxRingPhysAddr` | `0x03684800` | receive descriptors |
| `RingSizes` | `0x0007000A` | 8 receive, 11 send, packed (rx-1) << 16 \| (tx-1) |
| `AdapterControl` | `0x01040000`, then `0x01140000` | |
| `MIISpeed` | `0x105` | |
| `LinkSpeed` | `0x00010064` | |
| `TransmitterControl`, `ReceiverControl` | `1` | start |
| `TxRxControl` | `3` | |
| `MIIMask` | `8` | link change |
| `IrqMask` | `0x5F` | |

**Sending.** XNet writes `TxRxControl = 1` (kick) every time it queues a
frame, about 500 times a second while searching, from `sub_0020F96F`,
`sub_0020FA9C` and `sub_0020FACA`. Nothing ever completes, so the ring stays
full: 7 of 11 descriptors queued.

**Descriptors** are 8 bytes: a 32-bit buffer physical address, a 16-bit
length minus one, and 16 bits of flags.

```
send    [0] buffer 0x011FE8F2 length 0x0041 flags 0x8001   66-byte frame, valid | last
receive [0] buffer 0x03685002 length 0x07FD flags 0x8000   2 KB buffer, owned by the card
```

**The frames** are XNet's search for a host: Ethernet broadcast from this
console's address, IPv4 from 0.0.0.1 to 255.255.255.255, **UDP port 3074 to
3074**. There is no DHCP: on System Link XNet sends straight away, so there is
no address wait to answer.

```
dst FF:FF:FF:FF:FF:FF src 00:50:F2:57:AD:18 type 0800: 45 00 00 34 ... 40 11 ... 00 00 00 01 FF FF FF FF 0C 02 0C 02 ...
```

## Physical addresses: two storages

This matters for the model more than anything above. In this runtime, low
RAM and the contiguous window at 0x80000000 are **separate storage**
(`xbox_memory_layout.c`, kept apart on purpose so pinned pools do not land on
the XBE image), and `MmGetPhysicalAddress` answers a low VA with itself and a
window VA with VA - 0x80000000. So a physical address alone does not say
which storage it came from.

XNet uses both. The rings come from `MmAllocateContiguousMemory` and read
correctly through the window. The **frame buffers are in low RAM**
(0x011FE8F2 is XNet's pool): read through the window they are another
allocator's `mem_mark` fill, read through low RAM they are the frames above.
The APU reads every physical address through the window, which is right for
its buffers and would be wrong here.

The rule the card model needs: a physical address inside a live contiguous
allocation is the window, anything else is low RAM. The contiguous arena
already tracks its live blocks (`xbox_ContiguousFree` knows them), so this is
a query on the arena, not a new table.

## What the card model needs, from this

- `TxRxControl` bit 3 (IDLE) set after reset.
- On a kick: walk the send ring from where it last stopped, take each
  descriptor with bit 15 set, copy `length + 1` bytes from the buffer, give
  the frame to the tunnel, clear bit 15, and raise the send interrupt.
- On a received frame: copy it into the next receive descriptor with bit 15
  set, write the length, clear bit 15, and raise the receive interrupt.
- Interrupt status bits that the guest clears by writing 1, and the line
  raised on vector 0 through `kernel_raise_interrupt`, from the timer thread
  as the APU's is.
- Physical addresses resolved by the rule above.
