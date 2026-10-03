# System Link over the internet

Oct 2, 2026 · @phobos665

## Summary

The proposal is to give the recompiled game a virtual network card and send its Ethernet frames to the other players over UDP, through a small relay server. The game's own network code runs unchanged, so this works for every System Link game without per-game work.

This is the same approach xemu and XLink Kai use, which means it is proven, and a recompiled build could also join a game hosted in xemu. On Windows it needs only ordinary UDP sockets: no Npcap, no TAP driver and no admin rights.

Five of our fifteen title projects support System Link: TimeSplitters 2, TimeSplitters: Future Perfect, OutRun 2, MechAssault and Tony Hawk's Pro Skater 2x. The biggest risk is internet latency. These games were built for a LAN, and how well each one copes with 30 to 100 ms has to be found by testing.

The same virtual card also opens two more services. With an optional bridge mode it can join XLink Kai, which carries System Link games between real consoles and xemu. With a NAT mode, a player's own console identity and an account registered in xemu, it can sign in to Insignia, the Xbox Live replacement. OutRun 2 and MechAssault are both on Insignia's list.

## How System Link works on a real Xbox

System Link is plain Ethernet between consoles. Two Xboxes join with a crossover cable, or more through a hub or switch. Whatever the game sends goes out of the console's 100 Mbit network port as Ethernet frames.

- **The network stack is inside the game.** Microsoft's XNet library is statically linked into each XBE, the same way D3D8 is. The kernel only reports whether a cable is plugged in (`PhyGetLinkState`, `PhyInitialize`). XNet drives the network card's registers itself, at 0xFEF00000.
- **Games find each other by broadcast.** A client sends a broadcast on the local network asking for sessions, and each host answers. This is why System Link needs every console on one LAN: broadcasts do not cross the internet.
- **Traffic is encrypted by XNet.** The host creates a session key and hands it out in its answer. The clients register it, and XNet runs a key exchange before any game traffic flows. Only the game can read its own packets.
- **Consoles are identified by MAC address.** XNet's address for a console (`XNADDR`) includes its Ethernet MAC address, so two consoles with the same MAC cannot both be on one System Link game.

For us this means the right place to step in is the Ethernet frame. Everything above it is the game's own lifted code and should just work. Everything below it is simple: a frame is a byte array of at most 1,514 bytes.

## What xboxrecomp has today

There is no networking yet. The XNET section of each game is lifted and runs, but the hardware under it does nothing.

| Piece | Today | Where |
| --- | --- | --- |
| Link state | Always reports a cable plugged in, so games offer System Link | `src/kernel/kernel_xbox.c` (`xbox_PhyGetLinkState`) |
| Network card registers | Plain RAM with no behaviour. Frames the game queues are never sent, and nothing is ever received | `src/kernel/xbox_memory_layout.c` (MCPX span, NIC at 0xFEF00000) |
| MAC address | Fixed at 00:50:F2:00:00:01 for every install. Two players would collide | `src/kernel/kernel_xbox.c` (`XC_FACTORY_ETHERNET_ADDR`) |
| Interrupts | The game's interrupt routines are recorded by vector, and a device model can look them up and raise them | `src/kernel/kernel_bridge.c` (`xbox_GetConnectedInterrupt`) |
| Register traps | The memory watchpoint tool can fault on access to a page and name the code that touched it | `src/kernel/xbox_watchpoint.c` |
| Clock | `KeTickCount` ticks, which XNet's start-up timeouts rely on | `src/kernel/xbox_memory_layout.c` |

`docs/technical/xbox-game-enhancements.md` (section 6) already says System Link should come before any Xbox Live work, because it is toolkit work and helps every game that has it.

These are the title projects that support System Link, according to [Wikipedia's list](https://en.wikipedia.org/wiki/List_of_Xbox_System_Link_games):

| Title | Players in total | Players per console |
| --- | --- | --- |
| TimeSplitters 2 | 16 | 4 |
| TimeSplitters: Future Perfect | 16 | 4 |
| OutRun 2 | 8 | 1 |
| MechAssault | 8 | 2 |
| Tony Hawk's Pro Skater 2x | 8 | 1 |

The other ten (Black, Burnout 2, Dino Crisis 3, Gauntlet, GunValkyrie, Jet Set Radio Future, Max Payne, MKDA, Marvel vs. Capcom 2, Panzer Dragoon Orta) are not on the list.

## Options considered

There are three places we could connect the game to the host's network. I recommend option A.

| Option | How it works | Per-game work | Works with xemu and real Xboxes | Main cost |
| --- | --- | --- | --- | --- |
| **A. Emulate the network card** (recommended) | Give the registers at 0xFEF00000 real behaviour, as xemu does. Frames the game sends are copied out; frames from other players are copied in and the game's interrupt is raised | None | Yes, by carrying the same frames | Writing the card model: registers, the frame lists in guest memory, interrupts |
| B. Replace XNet's card driver | Use HLE on the functions inside XNet that send and receive a frame, the way `hle_d3d8.c` replaces D3D8 functions | One signature set per XDK version | Yes | Finding those internal functions in every XDK build, and calling the receive path from the right thread |
| C. Replace XNet's socket API | Map the game's socket calls to Winsock, as Cxbx-Reloaded does | High: each game uses XNet differently | No | XNet's encryption, keys and addresses have to be faked, and discovery broadcasts still need a relay |

Option A is the slowest kind of emulation, but that does not matter here: a game sends a 100 Mbit card very little traffic compared with the work of drawing a frame, so the cost should not show up in frame time. That needs measuring, but the work per packet is small. It also keeps the boundary where xemu has already proved it works.

Option B is the fallback if trapping register access in option A turns out to be too awkward.

We do not need to build the internet side from nothing either. xemu's [UDP tunnel mode](https://xemu.app/docs/networking/) puts each frame the emulated card sends into a UDP datagram and forwards it to a remote host. If we use the same format, xemu becomes a test partner for free. [XLink Kai](https://www.teamxlink.co.uk/) does the same job for real consoles, and is worth supporting later.

## Proposed design

There are four new pieces: a virtual network card, a unique MAC address per install, a tunnel client inside the runtime, and a relay server that players connect to.

&#91;embedded content: System Link over the internet · one frame, two PCs, one relay\]

The game and XNet are the title's own lifted code and do not change. Only the card, the tunnel client and the relay are new, and the relay is never needed when two players can reach each other directly.

### 1. Virtual network card

This is a model of the Xbox's built-in network card, in a new `src/net` folder. It is the largest piece of work. xemu's model of the same card is the reference for how it behaves.

- **Sending.** The game puts a frame in guest memory and tells the card to send it. The model copies the frame out and gives it to the tunnel client.
- **Receiving.** When a frame arrives, the model copies it into a buffer the game has given the card, marks the buffer as filled, and raises the game's network interrupt through `xbox_GetConnectedInterrupt`.
- **Seeing register access.** The card's registers do things when they are read or written. For example, writing a 1 to an interrupt bit clears it. Today the register page is plain RAM, so the model cannot see those accesses. The cheapest fix is for the lifter to send memory accesses through a checking function, but only in functions from the XNET section, where the driver lives. That code is small and not performance-critical. The fallback is to protect the page and catch each access, as the watchpoint tool does.

### 2. A unique MAC address

Every install now reports the same MAC address, 00:50:F2:00:00:01. Each install should pick a random one on first run, keep Microsoft's 00:50:F2 prefix, and save it in the settings file. A switch such as `RECOMP_SYSLINK_MAC` lets two copies on one PC use different addresses for testing. This is a small change and can be made first.

### 3. Tunnel client

This is a host thread inside the runtime with one UDP socket. It sends each frame the card transmits as one datagram, straight away, with no batching, so it adds no delay. It hands each datagram it receives to the card.

It is set with three switches, which the launcher can show later:

| Switch | Example | Meaning |
| --- | --- | --- |
| `RECOMP_SYSLINK` | `off`, `loopback`, `relay` | Off (the default), two copies on one PC, or over the internet |
| `RECOMP_SYSLINK_RELAY` | `relay.example.org:3075` | The relay server to use |
| `RECOMP_SYSLINK_ROOM` | `ts2-friday` | The room to join. Everyone in a room is on one virtual LAN |

### 4. Relay server

This is a small standalone program that anyone can host. It behaves like an Ethernet switch for each room. It learns which MAC address is behind which player, sends a frame meant for one console to that player only, and sends broadcast frames to everyone in the room.

The relay never needs to read the frames, and it cannot, because XNet encrypts them. It knows only who is in which room.

When XNet starts, it asks for an IP address with DHCP. On a LAN with no DHCP server it waits, then picks an address itself. The relay could answer DHCP to skip that wait. How long the wait is still needs measuring.

Later, the relay can introduce two players to each other so that they send frames directly to each other (UDP hole punching), with the relay kept as the fallback. A direct path is often much shorter than one through a relay.

### What this needs on Windows

- **Only ordinary UDP sockets.** It needs no Npcap, no TAP adapter, no driver install and no admin rights. xemu's bridged mode, by comparison, needs Npcap.
- **Windows Firewall** may ask once to allow the program, when it accepts direct connections from other players. Relay-only play sends traffic out first, which a home router and firewall normally allow.
- **The player's home network is never touched.** Frames go only to the relay and the other players, so the game cannot see or reach anything else on the player's LAN.
- **Other systems.** Because it is plain sockets, the same code should work on Linux and macOS as well.

## The hard part: latency

The tunnel can carry the frames, but it cannot remove the distance between players. Whether a game is playable over the internet depends on how its own network code copes with delay.

On a LAN, a packet takes under a millisecond and is almost never lost. Over the internet it typically takes tens of milliseconds, often 30 to 100 ms between players in different places, and some packets arrive late or not at all. The games were tested only on a LAN.

How a game copes depends on its design, and we cannot tell in advance:

- **One console runs the match and the others smooth the result.** These games usually cope. Halo: Combat Evolved has been played over XLink Kai for years this way.
- **Every console waits for every other one each step (lockstep).** The slowest link sets the pace for everyone, so the game may slow down or stutter.
- **A game drops a player whose packets are late.** If its timeout was tuned for a LAN, it may disconnect players on a slow link.

What we can do about it:

1. **Add no delay of our own.** Each frame goes out the moment the game sends it.
2. **Use the shortest path.** Connect players directly where possible, and use a relay close to them where not.
3. **Show it.** Add each player's ping to the F9 overlay, so players can see why a game stutters.
4. **Keep every PC at the same frame rate.** A game that ties its simulation to its frame rate may drift between a fast PC and a slow one. System Link should probably use the console's fixed rate (`RECOMP_FPS_CAP=60` or `30`) instead of the adaptive cap. This needs checking per game.
5. **Test delay on one PC.** The loopback mode can add an artificial delay and packet loss (for example `RECOMP_SYSLINK_DELAY_MS=80`), so each game can be tried at internet-like latency before anyone needs a second PC.
6. **Fix a game when it needs it.** Because we have the game's code, a timeout that is too short for the internet can be raised in that title's project. That is per-game work, done only for games that need it.

## Plan

The work splits into eight phases. Each one ends with something you can see working, and two players on one PC come before any internet work. XLink Kai and Insignia come last, because both build on the card model from the earlier phases. OutRun 2 is the first test title in this plan, because it has one player per console and is the title most recently worked on.

1. **Unique MAC and a register log.** Make the MAC address random per install. Use the watchpoint tool to log every access the game makes to the card's registers while XNet starts up.
   - Done when we have the list of registers OutRun 2 and TimeSplitters 2 touch, and in what order.
2. **Card model and loopback.** Build the card model and the `loopback` mode, which joins two copies on one PC.
   - Done when two copies of OutRun 2 on one PC find each other's System Link game and race.
3. **Check against xemu.** Connect a recompiled build to xemu's UDP tunnel.
   - Done when the recompiled build and xemu play one race together. This proves our card model behaves like xemu's.
4. **Relay server.** Write the relay and the `relay` mode.
   - Done when two PCs on different home networks play through the relay.
5. **Latency.** Add the artificial delay to loopback, ping to the F9 overlay, and check frame-rate pacing.
   - Done when each of the five System Link titles has a note saying how it plays at 50 ms and at 100 ms.
6. **Direct connections and the launcher.** Add hole punching, and room and relay fields in the launcher.
   - Done when two players can join from the launcher with a room code, and connect directly when their routers allow it.
7. **XLink Kai.** Add the optional `bridge` mode through Npcap.
   - Done when a recompiled build shows up in Kai's metrics tab and joins a System Link game with an xemu player through Kai.
8. **Insignia.** Add the NAT mode, load the console identity from `eeprom.bin`, and bring a registered account across from xemu.
   - Done when OutRun 2 signs in to Insignia and finds an online race. MechAssault is the second test.

## Insignia and XLink Kai

Insignia and XLink Kai do two separate jobs. XLink Kai carries System Link games over the internet. Insignia is a replacement for Xbox Live. Both can sit on the same virtual network card, so the card from the System Link work is needed for either.

| Service | What it replaces | Which game mode uses it | What a recompiled build needs |
| --- | --- | --- | --- |
| [XLink Kai](https://www.teamxlink.co.uk/) | A LAN between consoles | System Link | The card's frames on a host network adapter that Kai watches |
| [Insignia](https://insignia.live/) | Microsoft's Xbox Live servers, shut down in 2010 | Xbox Live | Internet access for the card, Insignia's DNS server, a registered console identity, and a Live account |

### Connecting to XLink Kai

The Kai program on a PC finds consoles by watching one of the PC's network adapters for their Ethernet frames. xemu joins in by [bridging its emulated card to that adapter](https://www.teamxlink.co.uk/wiki/Old_Xemu_and_XLink_Kai_Setup), which needs Npcap on Windows.

We can do the same thing with a `bridge` mode, which sends and receives the card's frames on a host adapter through Npcap. It would be optional, for players who want Kai, and the relay mode stays free of drivers.

Kai is worth supporting because it already has players and lobbies, and it would let a recompiled build play System Link with real consoles and with xemu players. One question to ask Team XLink: whether a local program can hand frames to Kai directly, without Npcap.

### Connecting to Insignia

Insignia works on real consoles and on xemu. It uses the original Xbox Live protocol, and supports 199 games ([Wikipedia](<https://en.wikipedia.org/wiki/Insignia_(Xbox)>), [Insignia](https://insignia.live/)). Of our titles, OutRun 2 and MechAssault are on [Insignia's games list](https://insignia.live/games). OutRun 2's Insignia entry has title ID 53450036, the same ID PR #34 now uses for its cache folder. TimeSplitters: Future Perfect, TimeSplitters 2 and Tony Hawk's Pro Skater 2x are not on the list.

Connecting needs five things:

1. **Internet access through the card.** System Link only needs a LAN, but Xbox Live talks to servers on the internet. The runtime needs a NAT (network address translation) mode inside it, like xemu's NAT mode, so the game's traffic goes out through the PC's normal connection. The NAT's built-in DHCP gives the game Insignia's DNS server, 46.101.64.175, as its primary, as [Insignia's connection guide](https://insignia.live/guide/connect) asks. The `bridge` mode above is a second way in: the game gets an address from the home router, the way a real console does.
2. **A real console identity.** Insignia registers each console with details from its EEPROM, and a console with a zeroed hard disk key cannot connect. Today we report a fixed serial number and a zeroed online key. The runtime should load the player's own `eeprom.bin` (the file xemu uses) and answer the serial number, MAC address, online key and hard disk key from it. That file holds secrets unique to one console, so it must never be committed or shared.
3. **Registration and an account, done once in xemu.** The player registers the console with the Insignia Setup disc, then signs up in the Xbox Live dashboard (version 1.00.5960.01) with a subscription code from Insignia's website. The account then has to be brought into our build. Where Xbox Live accounts are stored, and how to copy one into our FATX partition images, is an open question.
4. **The game's Live code has to run.** The XONLINE library is lifted and runs today, but TimeSplitters: Future Perfect stops at unresolved calls in its network set-up. Expect discovery work (seeds and indirect calls) in each title's Live code.
5. **A correct clock.** Xbox Live signs in with Kerberos, which rejects a client whose clock is far off. The kernel's system time must be the real time.

## Risks and open questions

- **Seeing register access.** Can the lifter route memory accesses through a check in XNET functions only? This depends on how it emits memory access, which still needs looking at. If it cannot, we fall back to page traps, which are slower and harder to get right.
- **Raising the interrupt safely.** The guest runs as one cooperative thread. A frame arrives on a host thread, so the card's interrupt has to be delivered to the game at a safe point, the way other device interrupts are delivered today. How that works needs confirming before phase 2.
- **Large frames.** A full 1,514-byte frame plus 28 bytes of IP and UDP headers is more than the usual 1,500-byte internet packet size, so the largest frames will be split in two on the way. This is probably fine, but we should measure how many frames are that large.
- **xemu's tunnel format.** We need to confirm that xemu sends the bare frame with nothing added before relying on it in phase 3.
- **Same game and build.** All players will need the same game version, and probably the same recompiled build. A mismatch should be reported clearly instead of failing silently.
- **Who runs a relay.** The relay is small and cheap to run, but someone has to host a public one. Self-hosting has to stay easy, with no accounts, only room codes.
- **Latency per game.** We do not know how each of the five titles plays at internet latency until phase 5.

This adds no new legal concern: no game code is shared, and the relay only ever carries encrypted frames between players who each own the game.

## Sources

- [List of Xbox System Link games](https://en.wikipedia.org/wiki/List_of_Xbox_System_Link_games), Wikipedia
- [xemu networking](https://xemu.app/docs/networking/), xemu documentation
- [XLink Kai](https://www.teamxlink.co.uk/), Team XLink
- [Insignia](https://insignia.live/), its [connection guide](https://insignia.live/guide/connect) and [games list](https://insignia.live/games)
- [Insignia (Xbox)](<https://en.wikipedia.org/wiki/Insignia_(Xbox)>), Wikipedia
- [Old Xemu and XLink Kai Setup](https://www.teamxlink.co.uk/wiki/Old_Xemu_and_XLink_Kai_Setup), Team XLink wiki
- In this repo: `docs/technical/xbox-game-enhancements.md` section 6, `src/kernel/kernel_xbox.c`, `src/kernel/kernel_bridge.c`, `src/kernel/xbox_memory_layout.c`
