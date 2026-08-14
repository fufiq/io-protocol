# DPI_RESISTANCE.md - how blockable this transport is, and why

> Written against the wire format as it stands. Every number below was measured
> from the code, not estimated: `noise_ik.hpp`'s `write_message1()` output plus
> `protocol.hpp`'s framing.

## The short version

**Blocking this is a half-hour job for anyone who decides to.** Not because it
is UDP, and not because of anything cryptographic - the crypto is fine - but
because of four fixed-offset plaintext artefacts in the framing.

The open question is not *can they* but *will they bother*, which is a function
of how visible the traffic becomes.

## What is on the wire

```
packet 1 (handshake init):   137 bytes, always
    offset 0     type            1 byte, value in {1..6}, plaintext
    offset 1     conn_id         8 bytes, caller-assigned
    offset 9     noise msg1      112 bytes (32 ephemeral + 48 enc static + 16 enc payload + 16 mac1)
    offset 121   mac2            16 bytes - ALL ZERO until a cookie has been received

packet 2 (handshake resp):    73 bytes
keepalive:                    33 bytes, every 20 s by default
cookie reply:                 25 bytes
```

## The four signatures, worst first

**1. Sixteen zero bytes at offset 121 of the first packet.** `mac2` is zero
until the responder has issued a cookie, which is every first connection
attempt. A constant run of zeros at a fixed offset in the first packet of a flow
is close to a perfect DPI rule: near-zero false positives, trivial to express,
no state required. This one is worse than the other three combined.

**2. A fixed 137-byte first packet.** WireGuard's is 148, and length is a large
part of how WireGuard is identified. Differing by a number rather than by a
principle is not a defence.

**3. A plaintext type byte at offset 0**, taking six known values. A one-line
classifier.

**4. A 33-byte keepalive every 20 seconds.** Behavioural, so it survives even if
the bytes become opaque. Constant size at a constant interval is a rhythm.

`conn_id` is caller-assigned and usually random-looking, so it is not a
signature by itself - but it is another eight bytes at a fixed offset that an
observer can use to correlate flows.

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

**1. Obfuscate the header and mac2.** The initiator already knows the
responder's static public key - Noise_IK requires it - so both sides can derive
an obfuscation key from it and encrypt the first nine bytes plus the mac2 field
under a per-packet nonce. That removes signatures 1, 3 and most of 4 in one
change, and leaves a first packet that is uniformly random to an observer who
does not already know the responder's key.

**2. Pad to variable length.** A random 0-255 byte tail on handshakes and
keepalives kills the length signature. Cheap, and it composes with everything
else.

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
