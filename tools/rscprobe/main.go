package main

import (
	"crypto/aes"
	"encoding/binary"
	"fmt"
	"os"
)

var ps3Key = []byte{0x85, 0x13, 0x6E, 0x1E, 0x37, 0xFC, 0xBC, 0x45, 0x94, 0xE7, 0xF7, 0xBC, 0x5F, 0x18, 0x52, 0x00, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33}

func rscSize(flags, page uint32) uint64 {
	chunk := uint64(page) << ((flags & 0xF) + 4)
	var t uint64
	t += uint64((flags>>4)&1) * (chunk >> 0)
	t += uint64((flags>>5)&3) * (chunk >> 1)
	t += uint64((flags>>7)&0xF) * (chunk >> 2)
	t += uint64((flags>>11)&0x3F) * (chunk >> 3)
	t += uint64((flags>>17)&0x7F) * (chunk >> 4)
	t += uint64((flags>>24)&1) * (chunk >> 5)
	t += uint64((flags>>25)&1) * (chunk >> 6)
	t += uint64((flags>>26)&1) * (chunk >> 7)
	t += uint64((flags>>27)&1) * (chunk >> 8)
	return t
}

func main() {
	f, _ := os.Open(os.Args[1])
	defer f.Close()
	hdr := make([]byte, 16)
	f.ReadAt(hdr, 0)
	ec := binary.BigEndian.Uint32(hdr[4:8])
	nf := binary.BigEndian.Uint32(hdr[8:12])
	toc := make([]byte, ec*16)
	f.ReadAt(toc, 0x10)
	blk, _ := aes.NewCipher(ps3Key)
	for i := 0; i+16 <= len(toc); i += 16 { blk.Decrypt(toc[i:i+16], toc[i:i+16]) }
	names := make([]byte, nf&0x0FFFFFFF)
	f.ReadAt(names, 0x10+int64(ec)*16)
	for i := 0; i+16 <= len(names); i += 16 { blk.Decrypt(names[i:i+16], names[i:i+16]) }
	getName := func(o uint32) string {
		if int(o) >= len(names) { return "?" }
		e := int(o); for e < len(names) && names[e] != 0 { e++ }
		return string(names[o:e])
	}
	buf := make([]byte, 16)
	shown := 0
	for i := 0; i < int(ec) && shown < 4; i++ {
		b := toc[i*16 : i*16+16]
		off := (binary.BigEndian.Uint32(b[0:]) >> 8) & 0xFFFFFF
		res := binary.BigEndian.Uint32(b[0:])>>31 != 0
		if !res { continue }
		off &= 0x7FFFFF
		cs := (uint32(b[0]&0xFF) << 16) | ((binary.BigEndian.Uint32(b[4:]) >> 16) & 0xFFFF)
		nm := (binary.BigEndian.Uint32(b[4:]) & 0xFFFF)
		d2 := binary.BigEndian.Uint32(b[8:])
		d3 := binary.BigEndian.Uint32(b[12:])
		f.ReadAt(buf, int64(off)*512)
		raw := append([]byte{}, buf...)
		blk.Decrypt(buf, buf)
		fmt.Printf("%s\n  raw16=% x\n  dec16=% x (magic=%q)\n", getName(nm), raw, buf, string(buf[:4]))
		fmt.Printf("  d2=%08X d3=%08X  size(d2,vp)+size(d3,pp)=%d+%d=%d  csz=%d off=0x%X\n",
			d2, d3, rscSize(d2, 0x1000), rscSize(d3, 0x1580), rscSize(d2, 0x1000)+rscSize(d3, 0x1580), cs, off*512)
		fmt.Printf("  swap: size(d3,vp)+size(d2,pp)=%d\n", rscSize(d3, 0x1000)+rscSize(d2, 0x1580))
		shown++
	}
}
