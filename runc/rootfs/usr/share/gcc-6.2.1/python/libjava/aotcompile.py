# -*- python -*-

## Copyright (C) 2005, 2006, 2008 Free Software Foundation
## Written by Gary Benson <gbenson@redhat.com>
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.

import classfile
import copy
# The md5 module is deprecated in Python 2.5
try: 
    from hashlib import md5 
except ImportError: 
    from md5 import md5
import operator
import os
import sys
import cStringIO as StringIO
import zipfile

PATHS = {"make":   "/usr/bin/make",
         "gcj":    "/usr/bin/gcj",
         "dbtool": "/usr/bin/gcj-dbtool"}

MAKEFLAGS = []
GCJFLAGS = ["-fPIC", "-findirect-dispatch", "-fjni"]
LDFLAGS = ["-Wl,-Bsymbolic"]

MAX_CLASSES_PER_JAR = 1024
MAX_BYTES_PER_JAR = 1048576

MAKEFILE = "Makefile"

MAKEFILE_HEADER = '''\
GCJ = %(gcj)s
DBTOOL = %(dbtool)s
GCJFLAGS = %(gcjflags)s
LDFLAGS = %(ldflags)s

%%.o: %%.jar
	$(GCJ) -c $(GCJFLAGS) $< -o $@

TARGETS = \\
%(targets)s

all: $(TARGETS)'''

MAKEFILE_JOB = '''
%(base)s_SOURCES = \\
%(jars)s

%(base)s_OBJECTS = \\
$(%(base)s_SOURCES:.jar=.o)

%(dso)s: $(%(base)s_OBJECTS)
	$(GCJ) -shared $(GCJFLAGS) $(LDFLAGS) $^ -o $@

%(db)s: $(%(base)s_SOURCES)
	$(DBTOOL) -n $@ 64
	for jar in $^; do \\
            $(DBTOOL) -f $@ $$jar \\
                %(libdir)s/%(dso)s; \\
        done'''

ZIPMAGIC, CLASSMAGIC = "PK\x03\x04", "\xca\xfe\xba\xbe"

class Error(Exception):
    pass

class Compiler:
    def __init__(self, srcdir, libdir, prefix = None):
        self.srcdir = os.path.abspath(srcdir)
        self.libdir = os.path.abspath(libdir)
        if prefix is None:
            self.dstdir = self.libdir
        else:
            self.dstdir = os.path.join(prefix, self.libdir.lstrip(os.sep))

        # Calling code may modify these parameters
        self.gcjflags = copy.copy(GCJFLAGS)
        self.ldflags = copy.copy(LDFLAGS)
        self.makeflags = copy.copy(MAKEFLAGS)
        self.exclusions = []

    def compile(self):
        """Search srcdir for classes and jarfiles, then generate
        solibs and mappings databases for them all in libdir."""
        if not os.path.isdir(self.dstdir):
            os.makedirs(self.dstdir)
        oldcwd = os.getcwd()
        os.chdir(self.dstdir)
        try:            
            jobs = self.getJobList()
            if not jobs:
                raise Error, "nothing to do"
            self.writeMakefile(MAKEFILE, jobs)
            for job in jobs:
                job.writeJars()
            system([PATHS["make"]] + self.makeflags)
            for job in jobs:
                job.clean()
            os.unlink(MAKEFILE)
        finally:
            os.chdir(oldcwd)

    def getJobList(self):
        """Return all jarfiles and class collections in srcdir."""
        jobs = weed_jobs(find_jobs(self.srcdir, self.exclusions))
        set_basenames(jobs)
        return jobs

    def writeMakefile(self, path, jobs):
        """Generate a makefile to build the solibs and mappings
        databases for the specified list of jobs."""
        fp = open(path, "w")
        print >>fp, MAKEFILE_HEADER % {
            "gcj": PATHS["gcj"],
            "dbtool": PATHS["dbtool"],
            "gcjflags": " ".join(self.gcjflags),
            "ldflags": " ".join(self.ldflags),
            "targets": " \\\n".join(reduce(operator.add, [
                (job.dsoName(), job.dbName()) for job in jobs]))}
        for job in jobs:
            values = job.ruleArguments()
            values["libdir"] = self.libdir
            print >>fp, MAKEFILE_JOB % values
        fp.close()

def find_jobs(dir, exclusions = ()):
    """Scan a directory and find things to compile: jarfiles (zips,
    wars, ears, rars, etc: we go by magic rather than file extension)
    and directories of classes."""
    def visit((classes, zips), dir, items):
        for item in items:
            path = os.path.join(dir, item)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            magic = open(path, "r").read(4)
            if magic == ZIPMAGIC:
                zips.append(path)
            elif magic == CLASSMAGIC:
                classes.append(path)
    classes, paths = [], []
    os.path.walk(dir, visit, (classes, paths))
    # Convert the list of classes into a list of directories
    while classes:
        # XXX this requires the class to be correctly located in its heirachy.
        path = classes[0][:-len(os.sep + classname(classes[0]) + ".class")]
        paths.append(path)
        classes = [cls for cls in classes if not cls.startswith(path)]
    # Handle exclusions.  We're really strict about them because the
    # option is temporary in aot-compile-rpm and dead options left in
    # specfiles will hinder its removal.
    for path in exclusions:
        if path in paths:
            paths.remove(path)
        else:
            raise Error, "%s: path does not exist or is not a job" % path
    # Build the list of jobs
    jobs = []
    paths.sort()
    for path in paths:
        if os.path.isfile(path):
            job = JarJob(path)
        else:
            job = DirJob(path)
        if len(job.classes):
            jobs.append(job)
    return jobs

class Job:
    """A collection of classes that will be compiled as a unit."""
    
    def __init__(self, path):
        self.path, self.classes, self.blocks = path, {}, None
        self.classnames = {}

    def addClass(self, bytes, name):
        """Subclasses call this from their __init__ method for
        every class they find."""
        digest = md5(bytes).digest()
        self.classes[digest] = bytes
        self.classnames[digest] = name

    def __makeBlocks(self):
        """Split self.classes into chunks that can be compiled to
        native code by gcj.  In the majority of cases this is not
        necessary -- the job will have come from a jarfile which will
        be equivalent to the one we generate -- but this only happens
        _if_ the job was a jarfile and _if_ the jarfile isn't too big
        and _if_ the jarfile has the correct extension and _if_ all
        classes are correctly named and _if_ the jarfile has no
        embedded jarfiles.  Fitting a special case around all these
        conditions is tricky to say the least.

        Note that this could be called at the end of each subclass's
        __init__ method.  The reason this is not done is because we
        need to parse every class file.  This is slow, and unnecessary
        if the job is subsetted."""
        names = {}
        for hash, bytes in self.classes.items():
            try:
                name = classname(bytes)
            except:
                warn("job %s: class %s malformed or not a valid class file" \
                     % (self.path, self.classnames[hash]))
                raise
            if not names.has_key(name):
                names[name] = []
            names[name].append(hash)
        names = names.items()
        # We have to sort somehow, or the jars we generate 
        # We sort by name in a simplistic attempt to keep related
        # classes together so inter-class optimisation can happen.
        names.sort()
        self.blocks, bytes = [[]], 0
        for name, hashes in names:
            for hash in hashes:
                if len(self.blocks[-1]) >= MAX_CLASSES_PER_JAR \
                   or bytes >= MAX_BYTES_PER_JAR:
                    self.blocks.append([])
                    bytes = 0
                self.blocks[-1].append((name, hash))
                bytes += len(self.classes[hash])

    # From Archit Shah:
    #   The implementation and the documentation don't seem to match.
    #  
    #    [a, b].isSubsetOf([a]) => True
    #  
    #   Identical copies of all classes this collection do not exist
    #   in the other. I think the method should be named isSupersetOf
    #   and the documentation should swap uses of "this" and "other"
    #
    # XXX think about this when I've had more sleep...
    def isSubsetOf(self, other):
        """Returns True if identical copies of all classes in this
        collection exist in the other."""
        for item in other.classes.keys():
            if not self.classes.has_key(item):
                return False
        return True

    def __targetName(self, ext):
        return self.basename + ext

    def tempJarName(self, num):
        return self.__targetName(".%d.jar" % (num + 1))

    def tempObjName(self, num):
        return self.__targetName(".%d.o" % (num + 1))

    def dsoName(self):
        """Return the filename of the shared library that will be
        built from this job."""
        return self.__targetName(".so")

    def dbName(self):
        """Return the filename of the mapping database that will be
        built from this job."""
        return self.__targetName(".db")

    def ruleArguments(self):
        """Return a dictionary of values that when substituted
        into MAKEFILE_JOB will create the rules required to build
        the shared library and mapping database for this job."""
        if self.blocks is None:
            self.__makeBlocks()
        return {
            "base": "".join(
                [c.isalnum() and c or "_" for c in self.dsoName()]),
            "jars": " \\\n".join(
                [self.tempJarName(i) for i in xrange(len(self.blocks))]),
            "dso": self.dsoName(),
            "db": self.dbName()}

    def writeJars(self):
        """Generate jarfiles that can be native compiled by gcj."""
        if self.blocks is None:
            self.__makeBlocks()
        for block, i in zip(self.blocks, xrange(len(self.blocks))):
            jar = zipfile.ZipFile(self.tempJarName(i), "w", zipfile.ZIP_STORED)
            for name, hash in block:
                jar.writestr(
                    zipfile.ZipInfo("%s.class" % name), self.classes[hash])
            jar.close()

    def clean(self):
        """Delete all temporary files created during this job's build."""
        if self.blocks is None:
            self.__makeBlocks()
        for i in xrange(len(self.blocks)):
            os.unlink(self.tempJarName(i))
            os.unlink(self.tempObjName(i))

class JarJob(Job):
    """A Job whose origin was a jarfile."""

    def __init__(self, path):
        Job.__init__(self, path)
        self._walk(zipfile.ZipFile(path, "r"))

    def _walk(self, zf):
        for name in zf.namelist():
            bytes = zf.read(name)
            if bytes.startswith(ZIPMAGIC):
                self._walk(zipfile.ZipFile(StringIO.StringIO(bytes)))
            elif bytes.startswith(CLASSMAGIC):
                self.addClass(bytes, name)

class DirJob(Job):
    """A Job whose origin was a directory of classfiles."""

    def __init__(self, path):
        Job.__init__(self, path)
        os.path.walk(path, DirJob._visit, self)

    def _visit(self, dir, items):
        for item in items:
            path = os.path.join(dir, item)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            fp = open(path, "r")
            magic = fp.read(4)
            if magic == CLASSMAGIC:
                self.addClass(magic + fp.read(), name)
    
def weed_jobs(jobs):
    """Remove any jarfiles that are completely contained within
    another.  This is more common than you'd think, and we only
    need one nativified copy of each class after all."""
    jobs = copy.copy(jobs)
    while True:
        for job1 in jobs:
            for job2 in jobs:
                if job1 is job2:
                    continue
                if job1.isSubsetOf(job2):
                    msg = "subsetted %s" % job2.path
                    if job2.isSubsetOf(job1):
                        if (isinstance(job1, DirJob) and
                            isinstance(job2, JarJob)):
                            # In the braindead case where a package
                            # contains an expanded copy of a jarfile
                            # the jarfile takes precedence.
                            continue
                        msg += " (identical)"
                    warn(msg)
                    jobs.remove(job2)
                    break
            else:
                continue
            break
        else:
            break
        continue
    return jobs

def set_basenames(jobs):
    """Ensure that each jarfile has a different basename."""
    names = {}
    for job in jobs:
        name = os.path.basename(job.path)
        if not names.has_key(name):
            names[name] = []
        names[name].append(job)
    for name, set in names.items():
        if len(set) == 1:
            set[0].basename = name
            continue
        # prefix the jar filenames to make them unique
        # XXX will not work in most cases -- needs generalising
        set = [(job.path.split(os.sep), job) for job in set]
        minlen = min([len(bits) for bits, job in set])
        set = [(bits[-minlen:], job) for bits, job in set]
        bits = apply(zip, [bits for bits, job in set])
        while True:
            row = bits[-2]
            for bit in row[1:]:
                if bit != row[0]:
                    break
            else:
                del bits[-2]
                continue
            break
        set = zip(
            ["_".join(name) for name in apply(zip, bits[-2:])],
            [job for bits, job in set])
        for name, job in set:
            warn("building %s as %s" % (job.path, name))
            job.basename = name
    # XXX keep this check until we're properly general
    names = {}
    for job in jobs:
        name = job.basename
        if names.has_key(name):
            raise Error, "%s: duplicate jobname" % name
        names[name] = 1

def system(command):
    """Execute a command."""
    status = os.spawnv(os.P_WAIT, command[0], command)
    if status > 0:
        raise Error, "%s exited with code %d" % (command[0], status)
    elif status < 0:
        raise Error, "%s killed by signal %d" % (command[0], -status)

def warn(msg):
    """Print a warning message."""
    print >>sys.stderr, "%s: warning: %s" % (
        os.path.basename(sys.argv[0]), msg)

def classname(bytes):
    """Extract the class name from the bytes of a class file."""
    klass = classfile.Class(bytes)
    return klass.constants[klass.constants[klass.name][1]][1]
