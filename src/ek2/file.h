/* eclean-kernel2
 * (c) 2016 Michał Górny
 * 2-clause BSD license
 */

#pragma once

#ifndef EK2_FILE_H
#define EK2_FILE_H 1

#include "ek2/util/relativepath.h"

#include <memory>

#include <ctime>

// A generic interface used to refer to a file on disk.
class File
{
protected:
	std::shared_ptr<RelativePath> path_;

	File(std::shared_ptr<RelativePath> path);

public:
	// get the filename (from relative path)
	std::string filename() const;
	// get the full path (also from relative path)
	std::string path() const;

	// get the textual name for the file type
	// (useful for pretty output)
	virtual const std::string& type() const = 0;

	// get the internal kernel version
	// returns empty string if not available
	virtual const std::string& version() const;

	// get the file identifying data
	FileID id() const;
	// get the file mtime
	time_t mtime() const;

	// remove the file
	virtual void remove() const;
};

// File subclass suitable for directories
class Directory : public File
{
public:
	Directory(std::shared_ptr<RelativePath> path);

	// remove the directory recursively
	virtual void remove() const;
};

#endif /*EK2_FILE_H*/
