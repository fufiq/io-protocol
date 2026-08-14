# DPI_RESISTANCE.md - how blockable this transport is, and why

> Written against the wire format as it stands. Every number below was measured
> from the code, not estimated: `noise_ik.hpp`'s `write_message1()` output plus
> `protocol.hpp`'s framing.

## The short version

**Blocking this was a half-hour job for anyone who decided to.** Not because it
is UDP, and not because of anything cryptographic - the crypto is fine - but
because of four fixed-offset plaintext artefacts in the framing. The worst one
- 16 zero bytes at a fixed offset in every flow's first packet - is closed now
(see item 1 below); the other three are not.

The open question is not *can they* but *will they bother*, which is a function
of how visible the traffic becomes.

## What is on the wire

```
packet 1 (handshake init):   137 bytes, always
    offset 0     type            1 byte - OBFUSCATED (see item 1): keystream, not a literal 1
    offset 1     conn_id         8 bytes - OBFUSCATED, same keystream
    offset 9     noise msg1      112 bytes (32 ephemeral + 48 enc static + 16 enc payload + 16 mac1) - plaintext structure, as Noise itself intends
    offset 121   mac2            16 bytes - OBFUSCATED: keystream when no cookie yet, XORed real mac2 on a retry; no longer distinguishably zero

packet 2 (handshake resp):    73 bytes, header plaintext (type=2, conn_id) - not covered, see item 1
keepalive:                    33 bytes, every 20 s by default, header plaintext (type=6, conn_id) - not covered
cookie reply:                 25 bytes, header plaintext (type=4, conn_id) - not covered
```

## The four signatures, worst first

**1. Sixteen zero bytes at offset 121 of the first packet - CLOSED.** `mac2`
used to be zero until the responder issued a cookie, on every first connection
attempt: a constant run of zeros at a fixed offset in the first packet of a
flow, close to a perfect DPI rule. It is now 16 bytes of ChaCha20 keystream,
which is what closed it - see "What to change" item 1 for the mechanism and its
stated scope (packet type 1 only, and why).

**2. A fixed 137-byte first packet - OPEN.** WireGuard's is 148, and length is a
large part of how WireGuard is identified. Differing by a number rather than by
a principle is not a defence. Item 1 obfuscated the CONTENT of packet 1, not
its length - this is unrelated work (item 2 below) and still to do.

**3. A plaintext type byte at offset 0, taking six known values - CLOSED for
packet 1, OPEN for packets 2-6.** A one-line classifier, for whichever packet
types it still applies to. Packet 1 (the flow-initiating packet, and the one a
signature-based rule is most likely written against) no longer has one; types
2 through 6 still do, for the reasons in item 1's scope note.

**4. A 33-byte keepalive every 20 seconds - OPEN.** Behavioural, so it survives
even if the bytes become opaque, and keepalive (type 6) is not one of the
packet types item 1 covers. Constant size at a constant interval is a rhythm;
closing it is item 3 below (jitter), unstarted.

`conn_id` is caller-assigned and usually random-looking, so it is not a
signature by itself - but for packet types 2 through 6 it is still another
eight bytes at a fixed offset that an observer can use to correlate flows.

## The environment this matters in

Russian TSPU equipment at operators blocks WireGuard and OpenVPN by handshake
signature, and throttles QUIC on some mobile networks. The direction of travel
over recent years has been from blacklists toward **whitelists** on mobile: pass
what is recognised, throttle or drop what is not.

That last point is the one that changes the strategy, and it is worth stating
plainly that the specifics here move faster than any document, vary by operator
and region, and should be re-checked rather than trusted from this page.

## Two strategies, and they are incompatible

- **Randomisation** - look like nothing. Every byte indistinguishable from
  random. Beats a blacklist; **loses to a whitelist**, where "unrecognised
  entropy" is itself the thing being dropped.
- **Mimicry** - look like QUIC or DTLS on 443. More work, survives whitelisting,
  and puts the traffic in a crowd rather than alone.

Under a whitelist regime mimicry is the only one that keeps working. Randomisation
is still worth doing first: it is much cheaper, and mimicry needs it anyway.

## What to change, by return on effort

**1. Obfuscate the header and mac2 - done, scoped to packet type 1.** The
initiator already knows the responder's static public key - Noise_IK requires
it - so both sides can derive an obfuscation key from it and encrypt the first
nine bytes plus the mac2 field under a per-packet nonce (`protocol.hpp`,
`xor_msg1_header` and the comment above it). That closes signature 1
completely: mac2 is no longer a run of zeros, it is 16 bytes of keystream
indistinguishable from the AEAD ciphertext next to it - checked on the wire by
`tests/test_protocol.cpp`'s `"a handshake init's header and mac2 do not look
like message1 on the wire"`, not just asserted in a comment. Signature 3 (the
plaintext type byte) is closed for this one packet type; signature 4
(keepalive rhythm) is untouched - that is item 3 below, a different packet.

The scope stopped at packet type 1 for a reason that turned out to matter more
than expected: **a responder has exactly one static key, so trying "is this an
obfuscated msg1" costs one fixed lookup regardless of connection count - every
other packet type's receiver would need to know *which* connection's key to
try, and conn_id (the answer) is exactly what obfuscating it would hide.**
Concretely, per type:

- **Type 2** (handshake response): the initiator has one key per pending
  connection, keyed by conn_id. Obfuscating type 2's header the same way would
  mean trying every pending connection's key against every incoming packet -
  O(pending handshakes) per packet, a real cost this project's own benchmarking
  culture would not want to bury.
- **Types 3 and 6** (data, keepalive): each has a free per-packet nonce already
  (the AEAD counter), but it is unique only within one connection's own send
  direction, restarting at 0 for every new connection. A key shared by every
  connection (the only kind a receiver could try without already knowing which
  connection a packet is) would let two different connections' headers be
  XORed together by anyone who captured both; a per-connection key reintroduces
  the type-2 lookup problem. Left open rather than shipped half-reasoned.
- **Types 4 and 5** (cookie reply, SACK): neither carries a nonce of its own,
  and SACKs are sent unauthenticated by design - inventing one for either would
  be new machinery bolted onto packets deliberately kept cheap and stateless.

One property worth stating rather than discovering later: a cookie retry
resends the exact same message1 bytes (same ephemeral key) with a freshly
computed mac2, so it reuses the same obfuscation keystream as the original
attempt - the two packets' headers come out byte-identical on the wire. That
links a retry to its original attempt, which source address and timing already
do for free, so it costs nothing beyond what a passive observer already had.
What must never repeat - the same keystream across two *different*
connections - cannot: every `connect()` draws its own ephemeral key.

**2. Pad to variable length.** A random 0-255 byte tail on handshakes and
keepalives kills the length signature. Cheap, and it composes with everything
else. Not yet done: item 1 leaves packet 1 at a fixed 137 bytes, still, and
that length is now the only exposed piece of it.

**3. Jitter the keepalive.** ±30% of the interval breaks the rhythm. Currently
`keepalive_interval_ms` is exact.

**4. Default to UDP/443.** Does nothing against DPI on its own, but removes the
free suspicion an unusual port attracts.

**5. Only then consider mimicry.** Wrapping in real QUIC, or emitting a
convincing QUIC Initial, is a different order of work and should be decided
after 1-4 are in and measured.

## What is already fine

Everything after the handshake is AEAD ciphertext with a fresh nonce per
transmission, so the data path has no plaintext structure to match on. The
handshake's cryptographic content is sound - the initiator's static key is
encrypted, the responder's identity never appears on the wire. **The exposure is
entirely in the framing this project added around Noise, not in Noise.**

That is the encouraging part: the fix is in one file's byte layout, not in the
protocol's design.
