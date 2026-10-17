package main

import (
	"crypto/aes"
	"encoding/binary"
	"fmt"
	"os"
)

func main() {
	exePath := os.Args[1]
	rpfPath := os.Args[2]
	rpf, err := os.ReadFile(rpfPath)
	if err != nil { panic(err) }
	exe, err := os.ReadFile(exePath)
	if err != nil { panic(err) }

	if string(rpf[0:4]) != "RPF7" { fmt.Println("not RPF7"); return }
	entryCount := binary.BigEndian.Uint32(rpf[4:8])
	namesField := binary.BigEndian.Uint32(rpf[8:12])
	tag := binary.BigEndian.Uint32(rpf[12:16])
	namesLen := namesField & 0x0FFFFFFF
	fmt.Printf("RPF7 BE entryCount=%d namesLen=%d tag=%08X\n", entryCount, namesLen, tag)

	tocOff := 0x10
	// two ciphertext blocks for validation: first 32 bytes of TOC
	ct := rpf[tocOff : tocOff+32]

	// filter: root dir entry => p[5..7]==7F FF FF and p[0..1]==00 00
	check := func(p []byte) bool {
		return p[0] == 0 && p[1] == 0 && p[5] == 0x7F && p[6] == 0xFF && p[7] == 0xFF
	}

	found := 0
	n := len(exe)
	buf := make([]byte, 16)
	for o := 0; o+32 <= n; o++ {
		// cheap entropy-ish pre-filter: skip windows of all-identical bytes or all printable
		b := exe[o : o+32]
		allSame := true
		allPrint := true
		for _, c := range b {
			if c != b[0] { allSame = false }
			if c < 0x20 || c > 0x7E { allPrint = false }
		}
		if allSame || allPrint { continue }

		blk, err := aes.NewCipher(b)
		if err != nil { continue }
		blk.Decrypt(buf, ct[0:16])
		if !check(buf) { continue }
		blk.Decrypt(buf, ct[16:32])
		if !check(buf) {
			// second entry may not satisfy name-offset==0; still report as weak hit
			fmt.Printf("WEAK  off=0x%X key=%x\n", o, b)
			found++
			continue
		}
		fmt.Printf("STRONG off=0x%X key=%x\n", o, b)
		found++
	}
	fmt.Println("candidates:", found)

	// also try word-swapped (BE<->LE) interpretation, useful if key stored as uint32 array
	fmt.Println("--- trying 32-bit word byte-swapped variants ---")
	for o := 0; o+32 <= n; o++ {
		b := exe[o : o+32]
		allSame := true
		for _, c := range b { if c != b[0] { allSame = false; break } }
		if allSame { continue }
		k := make([]byte, 32)
		for i := 0; i < 8; i++ {
			k[i*4+0] = b[i*4+3]; k[i*4+1] = b[i*4+2]; k[i*4+2] = b[i*4+1]; k[i*4+3] = b[i*4+0]
		}
		blk, err := aes.NewCipher(k)
		if err != nil { continue }
		blk.Decrypt(buf, ct[0:16])
		if check(buf) { fmt.Printf("SWAP STRONG off=0x%X key=%x\n", o, k) }
	}
}
