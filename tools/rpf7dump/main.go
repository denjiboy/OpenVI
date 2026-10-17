package main

import (
	"crypto/aes"
	"encoding/binary"
	"fmt"
	"os"
)

var ps3Key = []byte{0x85, 0x13, 0x6E, 0x1E, 0x37, 0xFC, 0xBC, 0x45, 0x94, 0xE7, 0xF7, 0xBC, 0x5F, 0x18, 0x52, 0x00, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33, 0xB3, 0x2A, 0x67, 0x30, 0x8C, 0xC1, 0xB8, 0x33}

func main() {
	f, _ := os.ReadFile(os.Args[1])
	entryCount := binary.BigEndian.Uint32(f[4:8])
	namesField := binary.BigEndian.Uint32(f[8:12])
	namesLen := namesField & 0x0FFFFFFF
	toc := make([]byte, entryCount*16)
	copy(toc, f[0x10:])
	blk, _ := aes.NewCipher(ps3Key)
	for i := 0; i < len(toc); i += 16 { blk.Decrypt(toc[i:i+16], toc[i:i+16]) }
	names := make([]byte, namesLen)
	copy(names, f[0x10+int(entryCount)*16:])
	for i := 0; i+16 <= len(names); i += 16 { blk.Decrypt(names[i:i+16], names[i:i+16]) }

	for i := 0; i < 12; i++ {
		o := i * 16
		b := toc[o : o+16]
		fmt.Printf("[%2d] %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x\n",
			i, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15])
		le0 := binary.LittleEndian.Uint32(b[0:])
		le1 := binary.LittleEndian.Uint32(b[4:])
		be0 := binary.BigEndian.Uint32(b[0:])
		be1 := binary.BigEndian.Uint32(b[4:])
		fmt.Printf("      LE: d0=%08X d1=%08X d2=%08X d3=%08X\n", le0, le1, binary.LittleEndian.Uint32(b[8:]), binary.LittleEndian.Uint32(b[12:]))
		fmt.Printf("      BE: d0=%08X d1=%08X d2=%08X d3=%08X\n", be0, be1, binary.BigEndian.Uint32(b[8:]), binary.BigEndian.Uint32(b[12:]))
		// show names at various u16 fields
		showName := func(off uint32) string {
			if int(off) >= len(names) { return "?" }
			e := int(off)
			for e < len(names) && names[e] != 0 { e++ }
			return string(names[off:e])
		}
		fmt.Printf("      name@LE16[0]=%q  name@BE16[0]=%q  name@LE16[6]=%q  name@BE16[6]=%q\n",
			showName(uint32(binary.LittleEndian.Uint16(b[0:]))), showName(uint32(binary.BigEndian.Uint16(b[0:]))),
			showName(uint32(binary.LittleEndian.Uint16(b[6:]))), showName(uint32(binary.BigEndian.Uint16(b[6:]))))
	}
	fmt.Printf("names[0:80]=%q\n", string(names[:80]))
}
