package main

import (
	"crypto/aes"
	"encoding/binary"
	"fmt"
	"os"
	"strings"
)

var ps3Key = []byte{0x85, 0x13, 0x6E, 0x1E, 0x37, 0xFC, 0xBC, 0x45, 0x94, 0xE7, 0xF7, 0xBC, 0x5F, 0x18, 0x52, 0x00, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33}
var ps3Key83 = []byte{0x83, 0x13, 0x6E, 0x1E, 0x37, 0xFC, 0xBC, 0x45, 0x94, 0xE7, 0xF7, 0xBC, 0x5F, 0x18, 0x52, 0x00, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33}

type Entry struct {
	d0, d1, d2, d3 uint32
}

func (e Entry) NameOff() uint32  { return e.d0 & 0xFFFF }
func (e Entry) OnDisk() uint32   { return (e.d0 >> 16) | ((e.d1 & 0xFF) << 16) }
func (e Entry) IsDir() bool      { return ((e.d1 >> 8) & 0x7FFFFF) == 0x7FFFFF }
func (e Entry) IsRes() bool      { return e.d1>>31 != 0 }
func (e Entry) Offset() uint32   { return ((e.d1 >> 8) & 0x7FFFFF) << 9 }
func (e Entry) Size() uint32     { return e.d2 }
func (e Entry) FirstIdx() uint32 { return e.d2 }
func (e Entry) Count() uint32    { return e.d3 }

func ecbDecrypt(key, data []byte) {
	blk, err := aes.NewCipher(key)
	if err != nil { panic(err) }
	n := len(data) - len(data)%16
	for i := 0; i < n; i += 16 { blk.Decrypt(data[i:i+16], data[i:i+16]) }
}

func main() {
	path := os.Args[1]
	key := ps3Key
	if len(os.Args) > 2 && os.Args[2] == "83" { key = ps3Key83 }
	f, err := os.ReadFile(path)
	if err != nil { panic(err) }
	if string(f[0:4]) != "RPF7" { fmt.Println("not RPF7"); return }
	be := func(o int) uint32 { return binary.BigEndian.Uint32(f[o : o+4]) }
	entryCount := be(4)
	namesField := be(8)
	tag := be(12)
	namesLen := namesField & 0x0FFFFFFF
	shift := (namesField >> 28) & 7
	fmt.Printf("entryCount=%d namesLen=%d shift=%d tag=%08X\n", entryCount, namesLen, shift, tag)

	toc := make([]byte, entryCount*16)
	copy(toc, f[0x10:0x10+int(entryCount)*16])
	ecbDecrypt(key, toc)
	names := make([]byte, namesLen)
	copy(names, f[0x10+int(entryCount)*16:])
	ecbDecrypt(key, names)

	entries := make([]Entry, entryCount)
	for i := range entries {
		o := i * 16
		entries[i] = Entry{binary.BigEndian.Uint32(toc[o:]), binary.BigEndian.Uint32(toc[o+4:]), binary.BigEndian.Uint32(toc[o+8:]), binary.BigEndian.Uint32(toc[o+12:])}
	}
	getName := func(e Entry) string {
		off := int(e.NameOff()) << shift
		if off >= len(names) { return "<bad>" }
		end := off
		for end < len(names) && names[end] != 0 { end++ }
		return string(names[off:end])
	}
	root := entries[0]
	fmt.Printf("root: dir=%v first=%d count=%d name=%q\n", root.IsDir(), root.FirstIdx(), root.Count(), getName(root))
	fmt.Printf("names[0:64]=%q\n", string(names[:64]))

	// print top-level + one level
	var walk func(idx int, depth int, prefix string)
	shown := 0
	walk = func(idx int, depth int, prefix string) {
		if shown > 60 { return }
		e := entries[idx]
		e.IsDir()
		if depth > 2 { return }
		for i := e.FirstIdx(); i < e.FirstIdx()+e.Count() && int(i) < len(entries); i++ {
			c := entries[i]
			nm := getName(c)
			kind := "FILE"
			if c.IsDir() { kind = "DIR " }
			if c.IsRes() { kind += "/RSC" }
			fmt.Printf("%s%s %-10s size=%-10d ondisk=%-8d off=0x%-8X %q\n", prefix, kind, "", c.Size(), c.OnDisk(), c.Offset(), nm)
			shown++
			if c.IsDir() && depth < 2 { walk(int(i), depth+1, prefix+"  ") }
		}
	}
	if root.IsDir() { walk(0, 0, "") } else { fmt.Println("ROOT NOT DIR -> wrong key") }
	_ = strings.TrimSpace
}
