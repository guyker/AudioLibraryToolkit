import argparse
import hashlib
import pathlib
import subprocess
import tempfile


def run(command):
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def proof(args, kind, path):
    log = subprocess.check_output([args.analyzer, "--proof", kind, str(path)], text=True)
    return next(line.split("\t") for line in log.splitlines() if line.startswith(("proof\t", "refused\t")))


def crc(data, width, polynomial):
    value = 0
    for byte in data:
        value ^= byte << (width - 8)
        for _ in range(8):
            value = (value << 1) ^ (polynomial if value & (1 << (width - 1)) else 0)
            value &= (1 << width) - 1
    return value.to_bytes(width // 8, "big")


def odd_depth_fixture():
    """One stereo verbatim frame, 20-bit signed samples, no stored MD5."""
    channels = [[-524288, -1, 0, 524287] * 4, [1, -3, 5, -7] * 4]
    pcm = b"".join(v.to_bytes(3, "little", signed=True) for pair in zip(*channels) for v in pair)
    frame = bytes.fromhex("ff f8 60 1a 00 0f")
    frame += crc(frame, 8, 0x07)
    for channel in channels:
        bits = "".join(f"{v & 0xfffff:020b}" for v in channel)
        frame += b"\x02" + int(bits, 2).to_bytes(len(bits) // 8, "big")
    frame += crc(frame, 16, 0x8005)
    params = (48000 << 44) | (1 << 41) | (19 << 36) | 16
    info = bytes.fromhex("00 10 00 10") + bytes(6) + params.to_bytes(8, "big") + bytes(16)
    return b"fLaC\x80\x00\x00\x22" + info + frame, pcm


def test_proof(args, root, clean, retagged, truncated):
    def verified(kind, path, bits, channels, pcm):
        observed = proof(args, kind, path)
        expected = [str(len(pcm) // (((bits + 7) // 8) * channels)), str(bits)]
        assert observed[0] == "proof" and observed[1:3] == expected, (path, observed)
        assert observed[4] == hashlib.sha256(pcm).hexdigest(), (path, observed)
        assert observed[5] == (hashlib.md5(pcm).hexdigest() if kind == "flac" else ""), (path, observed)
        return observed[3]

    def rendered(path, representation):
        return subprocess.check_output([args.ffmpeg, "-v", "error", "-i", str(path),
                                        "-map", "0:a:0", "-f", representation, "-"])

    pcm = rendered(clean, "s24le")
    assert verified("flac", clean, 24, 1, pcm) == verified("flac", retagged, 24, 1, pcm)
    odd = root / "odd-no-md5.flac"
    encoded, pcm = odd_depth_fixture()
    odd.write_bytes(encoded)
    verified("flac", odd, 20, 2, pcm)
    # A declared count mismatch is not a decoder failure (and must not clip observed samples).
    odd.write_bytes(encoded[:25] + b"\x20" + encoded[26:])
    verified("flac", odd, 20, 2, pcm)
    crc_error = root / "bad-footer.flac"
    crc_error.write_bytes(encoded[:-1] + bytes([encoded[-1] ^ 1]))
    assert proof(args, "flac", crc_error)[:4] == ["refused", "1", "42", str(len(encoded) - 42)]

    mp3 = root / "example.mp3"
    run([args.ffmpeg, "-i", str(clean), "-c:a", "libmp3lame", str(mp3)])
    verified("mp3", mp3, 32, 1, rendered(mp3, "f32le"))
    short_mp3 = root / "truncated.mp3"
    short_mp3.write_bytes(mp3.read_bytes()[:-3])
    assert proof(args, "mp3", short_mp3)[0] == "refused"
    suffix = root / "junk.flac"
    suffix.write_bytes(clean.read_bytes() + bytes(4186))
    result = proof(args, "flac", suffix)
    assert result[:2] == ["refused", "1"], result
    if result[2] != "-":
        end = int(result[2]) + int(result[3])
        assert clean.stat().st_size < end <= suffix.stat().st_size, result
    assert proof(args, "flac", truncated)[0] == "refused"
    assert proof(args, "flac", mp3)[:4] == ["refused", "0", "-", "-"]
    ogg = root / "ogg-with-flac-extension.flac"
    run([args.ffmpeg, "-i", str(clean), "-c:a", "copy", "-f", "ogg", str(ogg)])
    assert proof(args, "flac", ogg)[0] == "refused"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True)
    parser.add_argument("--ffmpeg", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="afc-decode-") as directory:
        root = pathlib.Path(directory)
        clean = root / "clean.flac"
        retagged = root / "retagged.flac"
        truncated = root / "truncated.flac"
        dual_mono = root / "dual-mono.flac"
        antiphase = root / "antiphase.flac"
        full_scale = root / "full-scale.flac"
        silence = root / "silence.flac"
        source_wav = root / "sixteen-bit.wav"
        padded = root / "padded-24-bit.flac"
        run([
            args.ffmpeg, "-y", "-f", "lavfi", "-i",
            "sine=frequency=997:sample_rate=48000:duration=2",
            "-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24",
            str(clean),
        ])
        run([
            args.ffmpeg, "-y", "-i", str(clean), "-c", "copy",
            "-metadata", "title=Retagged", str(retagged),
        ])
        data = clean.read_bytes()
        truncated.write_bytes(data[:-256])
        run([
            args.ffmpeg, "-y", "-f", "lavfi", "-i",
            "aevalsrc=0.1*sin(2*PI*997*t)|0.1*sin(2*PI*997*t):s=48000:d=2",
            "-c:a", "flac", str(dual_mono),
        ])
        run([
            args.ffmpeg, "-y", "-f", "lavfi", "-i",
            "aevalsrc=0.1*sin(2*PI*997*t)|-0.1*sin(2*PI*997*t):s=48000:d=2",
            "-c:a", "flac", str(antiphase),
        ])
        run([
            args.ffmpeg, "-y", "-f", "lavfi", "-i",
            "aevalsrc=1|1:s=48000:d=0.25", "-c:a", "flac",
            "-sample_fmt", "s32", "-bits_per_raw_sample", "24", str(full_scale),
        ])
        run([
            args.ffmpeg, "-y", "-f", "lavfi", "-i",
            "aevalsrc=if(between(t\\,0.2\\,0.8)\\,0.1*sin(2*PI*997*t)\\,0):s=48000:d=1",
            "-c:a", "flac", str(silence),
        ])
        run([
            args.ffmpeg, "-y", "-f", "lavfi", "-i",
            "sine=frequency=997:sample_rate=48000:duration=2",
            "-c:a", "pcm_s16le", str(source_wav),
        ])
        run([
            args.ffmpeg, "-y", "-i", str(source_wav), "-c:a", "flac",
            "-sample_fmt", "s32", "-bits_per_raw_sample", "24", str(padded),
        ])
        subprocess.run(
            [args.analyzer, str(clean), str(retagged), str(truncated),
             str(dual_mono), str(antiphase), str(padded), str(full_scale), str(silence)],
            check=True,
        )
        test_proof(args, root, clean, retagged, truncated)


if __name__ == "__main__":
    main()
