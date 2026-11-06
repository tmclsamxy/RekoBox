//go:build windows

package grpc_server

import "syscall"

const windowsStillActive = 259

// Windows 上不能用 os.Process.Wait 等父进程：Getppid 拿到的句柄不是本进程
// 创建的子进程，Wait 会一直阻塞或直接报错，导致守护逻辑形同虚设。
// 这里改为主动查询进程是否还活着。
func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	const processQueryLimitedInformation = 0x1000
	handle, err := syscall.OpenProcess(processQueryLimitedInformation, false, uint32(pid))
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(handle)
	var exitCode uint32
	if err = syscall.GetExitCodeProcess(handle, &exitCode); err != nil {
		return false
	}
	return exitCode == windowsStillActive
}
