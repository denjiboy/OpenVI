package main

import (
	"bytes"
	"compress/flate"
	"compress/zlib"
	"crypto/aes"
	"encoding/binary"
	"fmt"
	"io"
	"os"
)

var ps3Key = []byte{0x85, 0x13, 0x6E, 0x1E, 0x37, 0xFC, 0xBC, 0x45, 0x94, 0xE7, 0xF7, 0xBC, 0x5F, 0x18, 0x52, 0x00, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33}

type E struct{ b [16]byte }

func (e E) Off() uint32    { return uint32(e.b[0])<<16 | uint32(e.b[1])<<8 | uint32(e.b[2]) }
func (e E) ResBit() bool   { return e.b[0]&0x80 != 0 }
func (e E) CSz() uint32    { return uint32(e.b[3])<<16 | uint32(e.b[4])<<8 | uint32(e.b[5]) }
func (e E) Name() uint32   { return uint32(e.b[6])<<8 | uint32(e.b[7]) }
func (e E) Size() uint32   { return binary.BigEndian.Uint32(e.b[8:12]) }
func (e E) D3() uint32     { return binary.BigEndian.Uint32(e.b[12:16]) }
func (e E) IsDir() bool    { return e.Off() == 0x7FFFFF }

func main() {
	path := os.Args[1]
	f, err := os.Open(path)
	if err != nil { panic(err) }
	defer f.Close()
	hdr := make([]byte, 16)
	f.ReadAt(hdr, 0)
	entryCount := binary.BigEndian.Uint32(hdr[4:8])
	namesField := binary.BigEndian.Uint32(hdr[8:12])
	namesLen := namesField & 0x0FFFFFFF
	toc := make([]byte, entryCount*16)
	f.ReadAt(toc, 0x10)
	blk, _ := aes.NewCipher(ps3Key)
	for i := 0; i+16 <= len(toc); i += 16 { blk.Decrypt(toc[i:i+16], toc[i:i+16]) }
	names := make([]byte, namesLen)
	f.ReadAt(names, 0x10+int64(entryCount)*16)
	for i := 0; i+16 <= len(names); i += 16 { blk.Decrypt(names[i:i+16], names[i:i+16]) }
	getName := func(o uint32) string {
		if int(o) >= len(names) { return "?" }
		e := int(o)
		for e < len(names) && names[e] != 0 { e++ }
		return string(names[o:e])
	}
	fmt.Printf("== %s entryCount=%d namesLen=%d\n", path, entryCount, namesLen)
	if len(os.Args) > 2 && os.Args[2] == "ls" {
		depth := make([]int, 0)
		_ = depth
		var walk func(idx int, d int)
		shown := 0
		walk = func(idx int, d int) {
			e := E{[16]byte{}}
			copy(e.b[:], toc[idx*16:idx*16+16])
			for i := int(e.Size()); i < int(e.Size())+int(e.D3()); i++ {
				if i >= int(entryCount) || shown > 80 { return }
				c := E{[16]byte{}}
				copy(c.b[:], toc[i*16:i*16+16])
				nm := getName(c.Name())
				kind := "F"
				if c.IsDir() { kind = "D" }
				pad := ""
				for k := 0; k < d; k++ { pad += "  " }
				fmt.Printf("%s%s %-30s size=%-9d csz=%-8d off=0x%-9X d3=%d\n", pad, kind, nm, c.Size(), c.CSz(), c.Off()*512, c.D3())
				shown++
				if c.IsDir() && d < 2 { walk(i, d+1) }
			}
		}
		walk(0, 0)
		return
	}

	var nResBit, nRsc, nRscAndBit, nRscNoBit int
	var compressed, stored int
	var firstRsc string
	tmp := make([]byte, 4)
	for i := 0; i < int(entryCount); i++ {
		e := E{[16]byte{}}
		copy(e.b[:], toc[i*16:i*16+16])
		if e.IsDir() { continue }
		if e.CSz() != 0 { compressed++ } else { stored++ }
		if e.ResBit() { nResBit++ }
		f.ReadAt(tmp, int64(e.Off())*512)
		if bytes.Equal(tmp, []byte("RSC7")) {
			nRsc++
			if e.ResBit() { nRscAndBit++ } else { nRscNoBit++ }
			if firstRsc == "" {
				firstRsc = fmt.Sprintf("idx=%d name=%q off=0x%X size=%d csz=%d d3=%d resbit=%v", i, getName(e.Name()), e.Off()*512, e.Size(), e.CSz(), e.D3(), e.ResBit())
			}
		}
	}
	fmt.Printf("files: compressed=%d stored=%d | resbit set=%d | RSC7-magic hits=%d (with resbit=%d without=%d)\n", compressed, stored, nResBit, nRsc, nRscAndBit, nRscNoBit)
	if firstRsc != "" { fmt.Println("first RSC7:", firstRsc) }

	// decompress test: find first compressed non-resource-ish small file
	tested := 0
	for i := 0; i < int(entryCount) && tested < 4; i++ {
		e := E{[16]byte{}}
		copy(e.b[:], toc[i*16:i*16+16])
		if e.IsDir() || e.CSz() == 0 || e.CSz() > 1<<20 { continue }
		raw := make([]byte, e.CSz())
		f.ReadAt(raw, int64(e.Off())*512)
		dblk, _ := aes.NewCipher(ps3Key)
		dec := make([]byte, len(raw))
		copy(dec, raw)
		for j := 0; j+16 <= len(dec); j += 16 { dblk.Decrypt(dec[j:j+16], dec[j:j+16]) }
		raw2 := dec
		nm := getName(e.Name())
		var out []byte
		var how string
		{
			r := flate.NewReader(bytes.NewReader(raw))
			if o, err2 := io.ReadAll(io.LimitReader(r, 1<<24)); err2 == nil { out, how = o, "raw-deflate" }
			r.Close()
		}
		if out == nil {
			r := flate.NewReader(bytes.NewReader(raw2))
			if o, err2 := io.ReadAll(io.LimitReader(r, 1<<24)); err2 == nil { out, how = o, "aes+raw-deflate" }
			r.Close()
		}
		if out == nil {
			if r, err := zlib.NewReader(bytes.NewReader(raw)); err == nil {
				o, err2 := io.ReadAll(io.LimitReader(r, 1<<24))
				if err2 == nil { out, how = o, "zlib" }
			}
		}
		fmt.Printf("  [%d] %-24q csz=%-7d size=%-8d -> %s %d bytes; head=%q\n", i, nm, e.CSz(), e.Size(), how, len(out), firstN(out, 60))
		tested++
	}
}

func firstN(b []byte, n int) string {
	if len(b) < n { n = len(b) }
	return string(b[:n])
}
