"""Regression fixtures for the XMEye AVI H264/HEVC signature.
No frames or media files are stored here: only synthetic NAL headers.
"""
def hevc_header(packet):
    if len(packet) < 14 or packet[:4] != bytes.fromhex("00 00 00 fd"):
        return False
    if packet[8:12] != bytes.fromhex("00 00 00 01"):
        return False
    b0, b1 = packet[12:14]
    if (b0 & 0x81) or (b1 & 0xf8) or not (b1 & 7):
        return False
    t = (b0 >> 1) & 0x3f
    return t in (32, 33, 34) or 16 <= t <= 23

def hevc_parameter_sets(packet):
    if not hevc_header(packet) or (packet[12] >> 1) & 63 != 32:
        return False
    present = 0
    for i in range(8, min(len(packet), 4096)-5):
        if packet[i:i+4] != b"\x00\x00\x00\x01":
            continue
        b0, b1 = packet[i+4:i+6]
        if (b0 & 0x81) or (b1 & 0xf8) or not b1 & 7:
            continue
        t = (b0 >> 1) & 63
        if t in (32, 33, 34):
            present |= {32: 1, 33: 2, 34: 4}[t]
        if present == 7:
            return True
    return False

FD = bytes.fromhex("00 00 00 fd 00 75 55 6a")
SC = bytes.fromhex("00 00 00 01")
H264_SPS = FD + SC + bytes.fromhex("27 4d e0 28 8d 68 0e")
HEVC_KEY = FD + SC + bytes.fromhex("40 01 0c 01") + SC + bytes.fromhex("42 01 01") + SC + bytes.fromhex("44 01 01") + SC + bytes.fromhex("26 01 07")
HEVC_NO_PPS = FD + SC + bytes.fromhex("40 01 0c") + SC + bytes.fromhex("42 01 01")
H264_FAKE_IRAP = FD + SC + bytes.fromhex("26 01 0c 01")
H264_FAKE_VPS = FD + SC + bytes.fromhex("40 01 0c 01") + SC + bytes.fromhex("65 ff 30")
BAD_TID = FD + SC + bytes.fromhex("40 00 00") + SC + bytes.fromhex("42 01 01") + SC + bytes.fromhex("44 01 01")

def test_classifier():
    assert not hevc_header(H264_SPS)
    assert not hevc_parameter_sets(H264_SPS)
    assert hevc_header(HEVC_KEY)
    assert hevc_parameter_sets(HEVC_KEY)
    assert not hevc_parameter_sets(HEVC_NO_PPS)
    assert hevc_header(H264_FAKE_IRAP)
    assert not hevc_parameter_sets(H264_FAKE_IRAP)
    assert not hevc_parameter_sets(H264_FAKE_VPS)
    assert not hevc_header(BAD_TID)
    assert not hevc_parameter_sets(BAD_TID)
    print("OK: 10 XMEye AVI codec signature assertions")

if __name__ == "__main__":
    test_classifier()
