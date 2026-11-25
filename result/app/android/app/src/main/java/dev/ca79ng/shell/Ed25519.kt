package dev.ca79ng.shell

import java.math.BigInteger
import java.security.MessageDigest

/**
 * Ed25519 signature check (RFC 8032 section 5.1.7, the reference algorithm of section 6), for the signed update.json of
 * the online self-update. Self-contained because Android's platform providers only offer Ed25519 from API 33 and this
 * app runs from API 23. Verification only, public inputs only, so plain BigInteger arithmetic (not constant time) is fine.
 * Rejects: wrong lengths, a public key or R that is not a curve point, S >= L.
 */
object Ed25519 {
    private val P: BigInteger = BigInteger.ONE.shiftLeft(255).subtract(BigInteger.valueOf(19))
    private val L: BigInteger = BigInteger.ONE.shiftLeft(252).add(BigInteger("27742317777372353535851937790883648493"))
    private val TWO = BigInteger.valueOf(2)
    private val D: BigInteger = BigInteger.valueOf(-121665).multiply(inv(BigInteger.valueOf(121666))).mod(P)
    private val SQRT_M1: BigInteger = TWO.modPow(P.subtract(BigInteger.ONE).shiftRight(2), P)

    /** Extended homogeneous coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, x*y = T/Z. */
    private class Pt(val x: BigInteger, val y: BigInteger, val z: BigInteger, val t: BigInteger)

    private val G: Pt = run {
        val y = BigInteger.valueOf(4).multiply(inv(BigInteger.valueOf(5))).mod(P)
        val x = recoverX(y, 0)!!
        Pt(x, y, BigInteger.ONE, x.multiply(y).mod(P))
    }
    private val IDENTITY = Pt(BigInteger.ZERO, BigInteger.ONE, BigInteger.ONE, BigInteger.ZERO)

    private fun inv(a: BigInteger): BigInteger = a.modPow(P.subtract(TWO), P)

    private fun add(p: Pt, q: Pt): Pt {
        val a = p.y.subtract(p.x).multiply(q.y.subtract(q.x)).mod(P)
        val b = p.y.add(p.x).multiply(q.y.add(q.x)).mod(P)
        val c = p.t.multiply(TWO).multiply(D).multiply(q.t).mod(P)
        val d = p.z.multiply(TWO).multiply(q.z).mod(P)
        val e = b.subtract(a)
        val f = d.subtract(c)
        val g = d.add(c)
        val h = b.add(a)
        return Pt(e.multiply(f).mod(P), g.multiply(h).mod(P), f.multiply(g).mod(P), e.multiply(h).mod(P))
    }

    private fun mul(s: BigInteger, p: Pt): Pt {
        var q = IDENTITY
        var r = p
        for (i in 0 until s.bitLength()) {
            if (s.testBit(i)) q = add(q, r)
            r = add(r, r)
        }
        return q
    }

    private fun same(p: Pt, q: Pt): Boolean =
        p.x.multiply(q.z).subtract(q.x.multiply(p.z)).mod(P).signum() == 0 &&
            p.y.multiply(q.z).subtract(q.y.multiply(p.z)).mod(P).signum() == 0

    private fun recoverX(y: BigInteger, sign: Int): BigInteger? {
        if (y >= P) return null
        val y2 = y.multiply(y)
        val x2 = y2.subtract(BigInteger.ONE).multiply(inv(D.multiply(y2).add(BigInteger.ONE))).mod(P)
        if (x2.signum() == 0) return if (sign == 1) null else BigInteger.ZERO
        var x = x2.modPow(P.add(BigInteger.valueOf(3)).shiftRight(3), P)
        if (x.multiply(x).subtract(x2).mod(P).signum() != 0) x = x.multiply(SQRT_M1).mod(P)
        if (x.multiply(x).subtract(x2).mod(P).signum() != 0) return null
        if (x.testBit(0) != (sign == 1)) x = P.subtract(x)
        return x
    }

    /** Little-endian bytes as an unsigned integer. */
    private fun le(b: ByteArray, from: Int, to: Int): BigInteger {
        val r = ByteArray(to - from)
        for (i in r.indices) r[i] = b[to - 1 - i]
        return BigInteger(1, r)
    }

    private fun decompress(b: ByteArray, from: Int): Pt? {
        val sign = (b[from + 31].toInt() shr 7) and 1
        val c = b.copyOfRange(from, from + 32)
        c[31] = (c[31].toInt() and 0x7f).toByte()
        val y = le(c, 0, 32)
        val x = recoverX(y, sign) ?: return null
        return Pt(x, y, BigInteger.ONE, x.multiply(y).mod(P))
    }

    fun verify(pub: ByteArray, msg: ByteArray, sig: ByteArray): Boolean {
        if (pub.size != 32 || sig.size != 64) return false
        val a = decompress(pub, 0) ?: return false
        val r = decompress(sig, 0) ?: return false
        val s = le(sig, 32, 64)
        if (s >= L) return false
        val md = MessageDigest.getInstance("SHA-512")
        md.update(sig, 0, 32)
        md.update(pub)
        md.update(msg)
        val h = le(md.digest(), 0, 64).mod(L)
        return same(mul(s, G), add(r, mul(h, a)))
    }
}
