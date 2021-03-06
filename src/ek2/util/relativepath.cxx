/* eclean-kernel2
 * (c) 2016 Michał Górny
 * 2-clause BSD license
 */

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include "ek2/util/relativepath.h"

#include "ek2/util/directorystream.h"
#include "ek2/util/error.h"

#include <forward_list>
#include <stdexcept>
#include <vector>

#include <cassert>
#include <cerrno>

extern "C"
{
#	include <fcntl.h>
#	include <unistd.h>
};

OpenFD::OpenFD(int fd)
	: fd_(fd)
{
}

OpenFD::~OpenFD()
{
	if (fd_ != -1)
		close(fd_);
}

OpenFD::OpenFD(OpenFD&& other)
	: fd_(other.fd_)
{
	other.fd_ = -1;
}

OpenFD& OpenFD::operator=(OpenFD&& other)
{
	fd_ = other.fd_;
	other.fd_ = -1;
	return *this;
}

OpenFD::operator int() const
{
	return fd_;
}

void OpenFD::disown()
{
	fd_ = -1;
}

FileID::FileID(const struct stat& st)
	: dev_(st.st_dev), ino_(st.st_ino)
{
}

bool FileID::operator==(FileID other) const
{
	return dev_ == other.dev_ && ino_ == other.ino_;
}

std::hash<FileID>::result_type
std::hash<FileID>::operator()(argument_type id) const
{
	return std::hash<dev_t>()(id.dev_) ^ (std::hash<ino_t>()(id.ino_) << 8);
}

RelativePath::RelativePath(std::shared_ptr<DirectoryStream> dir,
			std::string&& filename)
	: dir_own_(dir), dir_(*dir), filename_(filename), file_fd_(-1)
{
	assert(!dir_.path_.empty());
}

RelativePath::RelativePath(const DirectoryStream& dir, std::string&& filename)
	: dir_(dir), filename_(filename), file_fd_(-1)
{
	assert(!dir_.path_.empty());
}

std::string RelativePath::filename() const
{
	return filename_;
}

std::string RelativePath::path() const
{
	if (!filename_.empty() && filename_[0] == '/')
		return filename_;
	return dir_.path_ + '/' + filename_;
}

OpenFD RelativePath::open(int flags) const
{
	int ret;

#if defined(HAVE_OPENAT)
	assert(dir_.dir_);
	ret = openat(dirfd(dir_.dir_), filename_.c_str(), flags);
#else
	ret = ::open(path().c_str(), flags);
#endif

	if (ret == -1)
		throw IOError("Unable to open " + path(), errno);
	return ret;
}

int RelativePath::file_fd(int flags)
{
	if (file_fd_ != -1 && flags != open_mode_)
		throw Error("Reopening not implemented yet");

	if (file_fd_ == -1)
	{
		file_fd_ = open(flags);
		open_mode_ = flags;
	}

	return file_fd_;
}

struct stat RelativePath::stat() const
{
	int ret;
	struct stat buf;

	if (file_fd_ != -1)
		ret = fstat(file_fd_, &buf);
	else
	{
#if defined(HAVE_FSTATAT)
		assert(dir_.dir_);
		ret = fstatat(dirfd(dir_.dir_), filename_.c_str(), &buf,
				AT_SYMLINK_NOFOLLOW);
#elif defined(HAVE_LSTAT)
		ret = lstat(path().c_str(), &buf);
#else
		ret = ::stat(path().c_str(), &buf);
#endif
	}

	if (ret == -1)
		throw IOError("Unable to stat " + path(), errno);

	return buf;
}

FileID RelativePath::id() const
{
	return FileID(stat());
}

std::string RelativePath::readlink() const
{
	std::vector<char> buf;

	if (file_fd_ != -1)
		throw Error("readlink() on open files not supported yet");

	// start with expected length (+ 1 for length check)
	buf.resize(stat().st_size + 1);

	while (true)
	{
		ssize_t ret;
#if defined(HAVE_READLINKAT)
		ret = readlinkat(dirfd(dir_.dir_), filename_.c_str(),
				buf.data(), buf.size());
#else
		ret = ::readlink(path().c_str(), buf.data(), buf.size());
#endif

		if (ret == -1)
			throw IOError("Unable to readlink " + path(), errno);
		// if we read up to buffer size, we don't know if there won't
		// be more, so increase buffer size and try again
		else if (ret == buf.size())
			buf.resize(buf.size() + 0x100);
		else
		{
			// truncate and return
			buf.resize(ret);
			break;
		}
	}

	return {buf.begin(), buf.end()};
}

void RelativePath::unlink() const
{
	int ret;

#if defined(HAVE_UNLINKAT)
	assert(dir_.dir_);
	ret = unlinkat(dirfd(dir_.dir_), filename_.c_str(), 0);
#else
	ret = ::unlink(path().c_str());
#endif

	if (ret == -1)
		throw IOError("Unable to unlink " + path(), errno);
}

void RelativePath::rmdir() const
{
	int ret;

#if defined(HAVE_UNLINKAT)
	assert(dir_.dir_);
	ret = unlinkat(dirfd(dir_.dir_), filename_.c_str(),
			AT_REMOVEDIR);
#else
	ret = ::rmdir(path().c_str());
#endif

	if (ret == -1)
		throw IOError("Unable to remove directory " + path(), errno);
}

void RelativePath::rmdir_recursive() const
{
	// (reversed stack)
	std::forward_list<DirectoryStream> stack;
	stack.emplace_front(*this);

	while (!stack.empty())
	{
		DirectoryStream& ds = stack.front();

		if (!stack.front().read())
		{
			stack.pop_front();

			// remove the now-empty directory
			if (!stack.empty())
			{
				DirectoryStream& dir = stack.front();
				RelativePath rp{dir, dir.filename()};
				try
				{
					rp.rmdir();
				}
				catch (const IOError& e)
				{
					if (e.err() != ENOENT)
						throw;
				}
			}
			continue;
		}

		// skip special entries
		if (ds.filename() == "." || ds.filename() == "..")
			continue;

		RelativePath rp{ds, ds.filename()};
		bool isdir = false;
		// is it a regular file, a symlink or anything unlinkable?
		try
		{
			rp.unlink();
		}
		catch (const IOError& e)
		{
			switch (e.err())
			{
				case ENOENT:
					// someone removed it for us? good enough
					break;
#ifdef EISDIR
				case EISDIR:
#endif
				case EPERM:
					// we can't unlink directories!
					isdir = true;
					break;
				default:
					throw;
			}
		}

		// a directory after all? recur!
		if (isdir)
			stack.emplace_front(rp);
	}

	// remove the top directory
	rmdir();
}
