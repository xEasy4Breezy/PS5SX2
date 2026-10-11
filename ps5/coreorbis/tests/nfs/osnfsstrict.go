// osnfsstrict: go-nfs serving a folder, refusing to mount any path but one (as a FreeBSD server exporting only that
// folder, without -alldirs, does). PS5SX2's NFS test (ps5/coreorbis/tests/nfs) uses it for the shorter-path mount.
//   osnfsstrict <folder> <address:port> <export path>
package main

import (
	"context"
	"fmt"
	"net"
	"os"

	billy "github.com/go-git/go-billy/v5"
	osfs "github.com/go-git/go-billy/v5/osfs"
	nfs "github.com/willscott/go-nfs"
	nfshelper "github.com/willscott/go-nfs/helpers"
)

type strict struct {
	nfs.Handler
	export string
}

func (s strict) Mount(ctx context.Context, c net.Conn, req nfs.MountRequest) (nfs.MountStatus, billy.Filesystem, []nfs.AuthFlavor) {
	status, fs, auths := s.Handler.Mount(ctx, c, req)
	if string(req.Dirpath) != s.export {
		// go-nfs makes a handle of the filesystem whatever the status (a nil one panics), and sends it only when OK.
		return nfs.MountStatusErrAcces, fs, auths
	}
	return status, fs, auths
}

func main() {
	if len(os.Args) != 4 {
		fmt.Printf("Usage: osnfsstrict <folder> <address:port> <export path>\n")
		os.Exit(2)
	}
	listener, err := net.Listen("tcp", os.Args[2])
	if err != nil {
		fmt.Printf("Failed to listen: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("osnfsstrict running at %s, exporting only %s\n", listener.Addr(), os.Args[3])
	handler := nfshelper.NewNullAuthHandler(osfs.New(os.Args[1]))
	fmt.Printf("%v", nfs.Serve(listener, nfshelper.NewCachingHandler(strict{handler, os.Args[3]}, 1024)))
}
