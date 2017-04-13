#include <uds/client_channel_factory.h>

#include <errno.h>
#include <log/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <uds/channel_manager.h>
#include <uds/client_channel.h>
#include <uds/ipc_helper.h>

using std::chrono::duration_cast;
using std::chrono::steady_clock;

namespace android {
namespace pdx {
namespace uds {

std::string ClientChannelFactory::GetRootEndpointPath() {
  return "/dev/socket/pdx";
}

std::string ClientChannelFactory::GetEndpointPath(
    const std::string& endpoint_path) {
  std::string path;
  if (!endpoint_path.empty()) {
    if (endpoint_path.front() == '/')
      path = endpoint_path;
    else
      path = GetRootEndpointPath() + '/' + endpoint_path;
  }
  return path;
}

ClientChannelFactory::ClientChannelFactory(const std::string& endpoint_path)
    : endpoint_path_{GetEndpointPath(endpoint_path)} {}

std::unique_ptr<pdx::ClientChannelFactory> ClientChannelFactory::Create(
    const std::string& endpoint_path) {
  return std::unique_ptr<pdx::ClientChannelFactory>{
      new ClientChannelFactory{endpoint_path}};
}

Status<std::unique_ptr<pdx::ClientChannel>> ClientChannelFactory::Connect(
    int64_t timeout_ms) const {
  Status<void> status;

  LocalHandle socket_fd{socket(AF_UNIX, SOCK_STREAM, 0)};
  if (!socket_fd) {
    ALOGE("ClientChannelFactory::Connect: socket error: %s", strerror(errno));
    return ErrorStatus(errno);
  }

  sockaddr_un remote;
  remote.sun_family = AF_UNIX;
  strncpy(remote.sun_path, endpoint_path_.c_str(), sizeof(remote.sun_path));
  remote.sun_path[sizeof(remote.sun_path) - 1] = '\0';

  bool use_timeout = (timeout_ms >= 0);
  auto now = steady_clock::now();
  auto time_end = now + std::chrono::milliseconds{timeout_ms};

  bool connected = false;
  int max_eaccess = 5;  // Max number of times to retry when EACCES returned.
  while (!connected) {
    int64_t timeout = -1;
    if (use_timeout) {
      auto remaining = time_end - now;
      timeout = duration_cast<std::chrono::milliseconds>(remaining).count();
      if (timeout < 0)
        return ErrorStatus(ETIMEDOUT);
    }
    ALOGD("ClientChannelFactory: Waiting for endpoint at %s", remote.sun_path);
    status = WaitForEndpoint(endpoint_path_, timeout);
    if (!status)
      return ErrorStatus(status.error());

    ALOGD("ClientChannelFactory: Connecting to %s", remote.sun_path);
    int ret = RETRY_EINTR(connect(
        socket_fd.Get(), reinterpret_cast<sockaddr*>(&remote), sizeof(remote)));
    if (ret == -1) {
      ALOGD("ClientChannelFactory: Connect error %d: %s", errno,
            strerror(errno));
      // if |max_eaccess| below reaches zero when errno is EACCES, the control
      // flows into the next "else if" statement and a permanent error is
      // returned from this function.
      if (errno == ECONNREFUSED || (errno == EACCES && max_eaccess-- > 0)) {
        // Connection refused/Permission denied can be the result of connecting
        // too early (the service socket is created but its access rights are
        // not set or not being listened to yet).
        ALOGD("ClientChannelFactory: %s, waiting...", strerror(errno));
        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(100ms);
      } else if (errno != ENOENT && errno != ENOTDIR) {
        // ENOENT/ENOTDIR might mean that the socket file/directory containing
        // it has been just deleted. Try to wait for its creation and do not
        // return an error immediately.
        ALOGE(
            "ClientChannelFactory::Connect: Failed to initialize connection "
            "when connecting: %s",
            strerror(errno));
        return ErrorStatus(errno);
      }
    } else {
      connected = true;
    }
    if (use_timeout)
      now = steady_clock::now();
  }  // while (!connected)

  ALOGD("ClientChannelFactory: Connected successfully to %s...",
        remote.sun_path);
  RequestHeader<BorrowedHandle> request;
  InitRequest(&request, opcodes::CHANNEL_OPEN, 0, 0, false);
  status = SendData(socket_fd.Borrow(), request);
  if (!status)
    return ErrorStatus(status.error());
  ResponseHeader<LocalHandle> response;
  status = ReceiveData(socket_fd.Borrow(), &response);
  if (!status)
    return ErrorStatus(status.error());
  int ref = response.ret_code;
  if (ref < 0 || static_cast<size_t>(ref) > response.file_descriptors.size())
    return ErrorStatus(EIO);

  LocalHandle event_fd = std::move(response.file_descriptors[ref]);
  return ClientChannel::Create(ChannelManager::Get().CreateHandle(
      std::move(socket_fd), std::move(event_fd)));
}

}  // namespace uds
}  // namespace pdx
}  // namespace android
