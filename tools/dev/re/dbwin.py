"""Capture OutputDebugString messages (like DbgView) for N seconds.
usage: dbwin.py <seconds> <outfile> [pid filter]"""
import ctypes, ctypes.wintypes as wt, sys, time, struct

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileMappingW.restype = wt.HANDLE
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.CreateEventW.restype = wt.HANDLE

secs = float(sys.argv[1])
out = open(sys.argv[2], "w", encoding="utf-8", errors="replace")
pid_filter = int(sys.argv[3]) if len(sys.argv) > 3 else None

ready = k32.CreateEventW(None, False, True, "DBWIN_BUFFER_READY")
data = k32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
mapping = k32.CreateFileMappingW(wt.HANDLE(-1), None, 4, 0, 4096, "DBWIN_BUFFER")
view = k32.MapViewOfFile(mapping, 4, 0, 0, 4096)
end = time.time() + secs
k32.SetEvent(ready)
while time.time() < end:
    if k32.WaitForSingleObject(data, 200) == 0:
        raw = ctypes.string_at(view, 4096)
        pid = struct.unpack_from("<I", raw, 0)[0]
        msg = raw[4:].split(b"\0", 1)[0].decode("utf-8", "replace")
        if pid_filter is None or pid == pid_filter:
            out.write(f"[{pid}] {msg.rstrip()}\n")
            out.flush()
        k32.SetEvent(ready)
out.close()
