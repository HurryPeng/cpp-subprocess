#include <map>
#include <iostream>
#include <string>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <sstream>
#include <memory>
#include <initializer_list>
#include <exception>

extern "C" {
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/wait.h>    
}

namespace subprocess {

using OutBuffer = std::vector<uint8_t>;
using ErrBuffer = std::vector<uint8_t>;

// Max buffer size allocated on stack for read error
// from pipe
static const size_t SP_MAX_ERR_BUF_SIZ = 1024;

// Exception Classes
class CalledProcessError: public std::runtime_error
{
public:
  CalledProcessError(const std::string& error_msg):
    std::runtime_error(error_msg)
  {}
};



class OSError: public std::runtime_error
{
public:
  OSError(const std::string& err_msg, int err_code):
    std::runtime_error( err_msg + " : " + std::strerror(err_code) )
  {}
};

//--------------------------------------------------------------------

namespace util
{
  static std::vector<std::string>
  split(const std::string& str, const std::string& delims=" \t")
  {
    std::vector<std::string> res;
    size_t init = 0;

    while (true) {
      auto pos = str.find_first_of(delims, init);
      if (pos == std::string::npos) {
	res.emplace_back(str.substr(init, str.length()));
	break;
      }
      res.emplace_back(str.substr(init, pos - init));
      pos++;
      init = pos;
    }

    return res;
  }

  static
  std::string join(const std::vector<std::string>& vec,
		   const std::string& sep = " ")
  {
    std::string res;
    for (auto& elem : vec) res.append(elem + sep);
    res.erase(--res.end());
    return res;
  }

  static
  void set_clo_on_exec(int fd, bool set = true)
  {
    int flags = fcntl(fd, F_GETFD, 0);
    if (set) flags |= FD_CLOEXEC;
    else flags &= ~FD_CLOEXEC;
    //TODO: should check for errors
    fcntl(fd, F_SETFD, flags);
  }

  static
  std::pair<int, int> pipe_cloexec()
  {
    int pipe_fds[2];
    int res = pipe(pipe_fds);
    if (res) {
      throw OSError("pipe failure", errno);
    }

    set_clo_on_exec(pipe_fds[0]);
    set_clo_on_exec(pipe_fds[1]);

    return std::make_pair(pipe_fds[0], pipe_fds[1]);
  }


  static
  int write_n(int fd, const char* buf, size_t length)
  {
    int nwritten = 0;
    while (nwritten < length) {
      int written = write(fd, buf + nwritten, length - nwritten);
      if (written == -1) return -1;
      nwritten += written;
    }
    return nwritten;
  }

  static
  int read_atmost_n(int fd, char* buf, size_t read_upto)
  {
    int rbytes = 0;
    int eintr_cnter = 0;

    while (1) {
      int read_bytes = read(fd, buf, read_upto);
      if (read_bytes == -1) {
      	if (errno == EINTR) {
      	  if (eintr_cnter >= 50) return -1;
      	  eintr_cnter++;
      	  continue;
	}
	return -1;
      }
      if (read_bytes == 0) return rbytes;

      rbytes += read_bytes;
    }
    return rbytes;
  }

  static
  int wait_for_child_exit(int pid)
  {
    int status;
    int ret = -1;
    while (1) {
      ret = waitpid(pid, &status, WNOHANG); 
      if (ret == -1) break;
      if (ret == 0) continue;
      return pid;
    }

    return ret;
  }


}; // end namespace util


// Popen Arguments
struct bufsize { 
  bufsize(int siz): bufsiz(siz) {}
  int  bufsiz = 0;
};

struct defer_spawn { 
  defer_spawn(bool d): defer(d) {}
  bool defer  = false;
};

struct close_fds { 
  close_fds(bool c): close_all(c) {}
  bool close_all = false;
};

struct string_arg
{
  string_arg(const char* arg): arg_value(arg) {}
  string_arg(std::string&& arg): arg_value(std::move(arg)) {}
  string_arg(std::string arg): arg_value(std::move(arg)) {}
  std::string arg_value;
};

struct executable: string_arg
{
  template <typename T>
  executable(T&& arg): string_arg(std::forward<T>(arg)) {}
};

struct cwd: string_arg
{
  template <typename T>
  cwd(T&& arg): string_arg(std::forward<T>(arg)) {}
};

struct environment
{
  environment(std::map<std::string, std::string>&& env):
    env_(std::move(env)) {}
  environment(const std::map<std::string, std::string>& env):
    env_(env) {}
  std::map<std::string, std::string> env_;
};

enum IOTYPE { 
  STDIN = 0,
  STDOUT,
  STDERR,
  PIPE,
};


struct input
{
  input(int fd): rd_ch_(fd) {}

  input(const char* filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) throw OSError("File not found: ", errno);
    rd_ch_ = fd;
  }
  input(IOTYPE typ) {
    assert (typ == PIPE);
    std::tie(rd_ch_, wr_ch_) = util::pipe_cloexec();
  }

  int rd_ch_ = -1;
  int wr_ch_ = -1;
};

struct output
{
  output(int fd): wr_ch_(fd) {}

  output(const char* filename) {
    int fd = open(filename, O_APPEND | O_CREAT | O_RDWR, 0640);
    if (fd == -1) throw OSError("File not found: ", errno);
    wr_ch_ = fd;
  }
  output(IOTYPE typ) {
    assert (typ == PIPE);
    std::tie(rd_ch_, wr_ch_) = util::pipe_cloexec();
  }

  int rd_ch_ = -1;
  int wr_ch_ = -1;
};

struct error
{
  error(int fd): wr_ch_(fd) {}

  error(const char* filename) {
    int fd = open(filename, O_APPEND | O_CREAT | O_RDWR, 0640); 
    if (fd == -1) throw OSError("File not found: ", errno);
    wr_ch_ = fd;
  }
  error(IOTYPE typ) {
    assert (typ == PIPE);
    std::tie(rd_ch_, wr_ch_) = util::pipe_cloexec();
  }

  int rd_ch_ = -1;
  int wr_ch_ = -1;
};

// ~~~~ End Popen Args ~~~~

// Fwd Decl.
class Popen;

namespace detail {

struct ArgumentDeducer
{
  ArgumentDeducer(Popen* p): popen_(p) {}

  void set_option(executable&& exe);
  void set_option(cwd&& cwdir);
  void set_option(bufsize&& bsiz);
  void set_option(environment&& env);
  void set_option(defer_spawn&& defer);
  void set_option(input&& inp);
  void set_option(output&& out);
  void set_option(error&& err);
  void set_option(close_fds&& cfds);

private:
  Popen* popen_ = nullptr;
};


class Child
{
public:
  Child(Popen* p, int err_wr_pipe):
    parent_(p),
    err_wr_pipe_(err_wr_pipe)
  {}

  void execute_child();

private:
  // Lets call it parent even though 
  // technically a bit incorrect
  Popen* parent_ = nullptr;
  int err_wr_pipe_ = -1;
};

class Streams
{
public:
  Streams() {}
  void operator=(const Streams&) = delete;

public:
  void setup_comm_channels();

  void cleanup_fds()
  {
    if (write_to_child_ != -1 && read_from_parent_ != -1) {
      close(write_to_child_);
    }
    if (write_to_parent_ != -1 && read_from_child_ != -1) {
      close(read_from_child_);
    }
    if (err_write_ != -1 && err_read_ != -1) {
      close(err_read_);
    }
  }

  void close_parent_fds()
  {
    if (write_to_child_ != -1)  close(write_to_child_);
    if (read_from_child_ != -1) close(read_from_child_);
    if (err_read_ != -1)        close(err_read_);
  }

  void close_child_fds()
  {
    if (write_to_parent_ != -1)  close(write_to_parent_);
    if (read_from_parent_ != -1) close(read_from_parent_);
    if (err_write_ != -1)        close(err_write_);
  }

public:// Yes they are public
  std::shared_ptr<FILE> input_  = nullptr;
  std::shared_ptr<FILE> output_ = nullptr;
  std::shared_ptr<FILE> error_  = nullptr;

  // Buffer size for the IO streams
  int bufsiz_ = 0;

  // Pipes for communicating with child

  // Emulates stdin
  int write_to_child_   = -1; // Parent owned descriptor
  int read_from_parent_ = -1; // Child owned descriptor

  // Emulates stdout
  int write_to_parent_ = -1; // Child owned descriptor
  int read_from_child_ = -1; // Parent owned descriptor

  // Emulates stderr
  int err_write_ = -1; // Write error to parent (Child owned)
  int err_read_  = -1; // Read error from child (Parent owned)
};

}; // end namespace detail


class Popen
{
public:
  friend class detail::ArgumentDeducer;
  friend class detail::Child;

  template <typename... Args>
  Popen(const std::string& cmd_args, Args&& ...args): 
    args_(cmd_args)
  {
    vargs_ = util::split(cmd_args);
    init_args(std::forward<Args>(args)...);

    if (!defer_process_start_) execute_process();
  }

  template <typename... Args>
  Popen(std::initializer_list<const char*> cmd_args, Args&& ...args)
  {
    vargs_.insert(vargs_.end(), cmd_args.begin(), cmd_args.end());
    init_args(std::forward<Args>(args)...);

    if (!defer_process_start_) execute_process();
  }

  void start_process() throw (CalledProcessError, OSError);

  int pid() const noexcept { return child_pid_; }

  void communicate(bool finish = false);

  void set_out_buf_cap();

  void set_err_buf_cap();

  OutBuffer& out_buf();

  ErrBuffer& err_buf();

private:
  template <typename F, typename... Args>
  void init_args(F&& farg, Args&&... args);
  void init_args();
  void populate_c_argv();
  void execute_process() throw (CalledProcessError, OSError);

private:
  detail::Streams stream_;

  bool defer_process_start_ = false;
  bool close_fds_ = false;
  std::string exe_name_;
  std::string cwd_;
  std::map<std::string, std::string> env_;

  // Command in string format
  std::string args_;
  // Comamnd provided as sequence
  std::vector<std::string> vargs_;
  std::vector<char*> cargv_;

  // Pid of the child process
  int child_pid_ = -1;
};

void Popen::init_args() {
  populate_c_argv();
}

template <typename F, typename... Args>
void Popen::init_args(F&& farg, Args&&... args)
{
  detail::ArgumentDeducer argd(this);
  argd.set_option(std::forward<F>(farg));
  init_args(std::forward<Args>(args)...);
}

void Popen::populate_c_argv()
{
  cargv_.reserve(vargs_.size());
  for (auto& arg : vargs_) cargv_.push_back(&arg[0]);
}

void Popen::start_process() throw (CalledProcessError, OSError)
{
  // The process was started/tried to be started
  // in the constructor itself.
  // For explicitly calling this API to start the
  // process, 'defer_spawn' argument must be set to
  // true in the constructor.
  if (!defer_process_start_) {
    assert (0);
    return;
  }
  execute_process();
}


void Popen::execute_process() throw (CalledProcessError, OSError)
{
  int err_rd_pipe, err_wr_pipe;
  std::tie(err_rd_pipe, err_wr_pipe) = util::pipe_cloexec();

  if (!exe_name_.length()) {
    exe_name_ = vargs_[0];
  }

  child_pid_ = fork();

  if (child_pid_ < 0) {
    close(err_rd_pipe);
    close(err_wr_pipe);
    throw OSError("fork failed", errno);
  }

  if (child_pid_ == 0)
  {
    // Close descriptors belonging to parent
    stream_.close_parent_fds();

    //Close the read end of the error pipe
    close(err_rd_pipe);

    detail::Child chld(this, err_wr_pipe);
    chld.execute_child();
  } 
  else 
  {
    int sys_ret = -1;
    close (err_wr_pipe);// close child side of pipe, else get stuck in read below

    stream_.close_child_fds();

    try {
      char err_buf[SP_MAX_ERR_BUF_SIZ] = {0,};
      int read_bytes = util::read_atmost_n(err_rd_pipe, err_buf, SP_MAX_ERR_BUF_SIZ);
      close(err_rd_pipe);

      if (read_bytes || strlen(err_buf)) {
	// Call waitpid to reap the child process
	// waitpid suspends the calling process until the
	// child terminates.
	sys_ret = util::wait_for_child_exit(child_pid_);
	if (sys_ret == -1) throw OSError("child exit", errno);

	// Throw whatever information we have about child failure
	throw CalledProcessError(err_buf);
      }
    } catch (std::exception& exp) {
      stream_.cleanup_fds();
      throw exp;
    }

    // Setup the communication channels of the Popen class
    stream_.setup_comm_channels();
  }
}

namespace detail {

  void ArgumentDeducer::set_option(executable&& exe) {
    popen_->exe_name_ = std::move(exe.arg_value);
  }

  void ArgumentDeducer::set_option(cwd&& cwdir) {
    popen_->cwd_ = std::move(cwdir.arg_value);
  }

  void ArgumentDeducer::set_option(bufsize&& bsiz) {
    popen_->stream_.bufsiz_ = bsiz.bufsiz;
  }

  void ArgumentDeducer::set_option(environment&& env) {
    popen_->env_ = std::move(env.env_);
  }

  void ArgumentDeducer::set_option(defer_spawn&& defer) {
    popen_->defer_process_start_ = defer.defer;
  }

  void ArgumentDeducer::set_option(input&& inp) {
    popen_->stream_.read_from_parent_ = inp.rd_ch_;
    if (inp.wr_ch_ != -1) popen_->stream_.write_to_child_ = inp.wr_ch_;
  }

  void ArgumentDeducer::set_option(output&& out) {
    popen_->stream_.write_to_parent_ = out.wr_ch_;
    if (out.rd_ch_ != -1) popen_->stream_.read_from_child_ = out.rd_ch_;
  }

  void ArgumentDeducer::set_option(error&& err) {
    popen_->stream_.err_write_ = err.wr_ch_;
    if (err.rd_ch_ != -1) popen_->stream_.err_read_ = err.rd_ch_;
  }

  void ArgumentDeducer::set_option(close_fds&& cfds) {
    popen_->close_fds_ = cfds.close_all;
  }


  void Child::execute_child() {
    int sys_ret = -1;
    auto& stream = parent_->stream_;

    try {
      if (stream.write_to_parent_ == 0)
      	stream.write_to_parent_ = dup(stream.write_to_parent_);

      if (stream.err_write_ == 0 || stream.err_write_ == 1)
      	stream.err_write_ = dup(stream.err_write_);

      // Make the child owned descriptors as the
      // stdin, stdout and stderr for the child process
      auto _dup2_ = [](int fd, int to_fd) {
        if (fd == to_fd) {
          // dup2 syscall does not reset the
          // CLOEXEC flag if the descriptors
          // provided to it are same.
          // But, we need to reset the CLOEXEC
          // flag as the provided descriptors
          // are now going to be the standard
          // input, output and error
          util::set_clo_on_exec(fd, false);
        } else if(fd != -1) {
          int res = dup2(fd, to_fd);
          if (res == -1) throw OSError("dup2 failed", errno);
        }
      };

      // Create the standard streams
      _dup2_(stream.read_from_parent_, 0); // Input stream
      _dup2_(stream.write_to_parent_,  1); // Output stream
      _dup2_(stream.err_write_,        2); // Error stream

      // Close the duped descriptors
      if (stream.read_from_parent_ != -1 && stream.read_from_parent_ > 2) 
      	close(stream.read_from_parent_);

      if (stream.write_to_parent_ != -1 && stream.write_to_parent_ > 2) 
      	close(stream.write_to_parent_);

      if (stream.err_write_ != -1 && stream.err_write_ > 2) 
      	close(stream.err_write_);

      // Close all the inherited fd's except the error write pipe
      if (parent_->close_fds_) {
        int max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd == -1) throw OSError("sysconf failed", errno);

        for (int i = 3; i < max_fd; i++) {
          if (i == err_wr_pipe_) continue;
          close(i);
        }
      }

      // Change the working directory if provided
      if (parent_->cwd_.length()) {
        sys_ret = chdir(parent_->cwd_.c_str());
        if (sys_ret == -1) throw OSError("chdir failed", errno);
      }

      // Replace the current image with the executable
      if (parent_->env_.size()) {
        for (auto& kv : parent_->env_) {
          setenv(kv.first.c_str(), kv.second.c_str(), 1);
	}
        sys_ret = execvp(parent_->exe_name_.c_str(), parent_->cargv_.data());
      } else {
        sys_ret = execvp(parent_->exe_name_.c_str(), parent_->cargv_.data());
      }

      if (sys_ret == -1) throw OSError("execve failed", errno);

    } catch (const OSError& exp) {
      // Just write the exception message
      // TODO: Give back stack trace ?
      std::string err_msg(exp.what());
      //ATTN: Can we do something on error here ?
      util::write_n(err_wr_pipe_, err_msg.c_str(), err_msg.length());
    }

    // Calling application would not get this
    // exit failure
    exit (EXIT_FAILURE);
  }


  void Streams::setup_comm_channels()
  {
    if (write_to_child_ != -1)  input_.reset(fdopen(write_to_child_, "wb"), fclose);
    if (read_from_child_ != -1) output_.reset(fdopen(read_from_child_, "rb"), fclose);
    if (err_read_ != -1)        error_.reset(fdopen(err_read_, "rb"), fclose);

    auto handles = {input_.get(), output_.get(), error_.get()};

    for (auto& h : handles) {
      if (h == nullptr) continue;
      switch (bufsiz_) {
      case 0:
	setvbuf(h, nullptr, _IONBF, BUFSIZ);
	break;
      case 1:
	setvbuf(h, nullptr, _IONBF, BUFSIZ);
	break;
      default:
	setvbuf(h, nullptr, _IOFBF, bufsiz_);
      };
    }
  }


}; // end namespace detail


};
