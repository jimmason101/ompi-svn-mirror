#!/usr/bin/env perl
#
# Copyright (c) 2008-2010 Cisco Systems, Inc.  All rights reserved.
#
# Dumb script to run through all the svn:ignore's in the tree and build
# build a .hgignore file for Mercurial.  Do a few trivial things to
# to try to have a few "global" files that don't need to be listed in
# every directory (i.e., reduce the total length of the .hgignore file).

use strict;

# Sanity check
die "Not in HG+SVN repository top dir"
    if (! -d ".hg" && ! -d ".svn");

# Put in some specials that we ignore everywhere
my @hgignore;
push(@hgignore, "# Automatically generated by build-hgignore.pl; edits may be lost!
syntax: glob");
my @globals = qw/.libs
.deps
.svn
*.la
*.lo
*.o
*.so
*.a
.dirstamp
*.dSYM
*.S
*.loT
*.orig
*.rej
*.class
*.xcscheme
*.plist
.git*
.DS_Store
stamp-h[1-9]
configure
config.guess
config.sub
config.log
config.status
libtool
ltmain.sh
missing
depcomp
install-sh
aclocal.m4
autom4te.cache
Makefile
static-components.h
project_list.m4
orte_wrapper_script
ompi_wrapper_script
make.out
config.out
auto.out
diff.out
*~
*\\\#/;

my $debug;
$debug = 1
    if ($ARGV[0]);

print "Thinking...\n"
    if (!$debug);

# Start at the top level
process(".");

# See if there's an .hgignore_local file.  If so, add its contents to the end.
if (-f ".hgignore_local") {
    open(IN, ".hgignore_local") || die "Can't open .hgignore_local";
    while (<IN>) {
        chomp;
        push(@globals, $_);
    }

    close(IN);
}

# If there's an old .hgignore, delete it
unlink(".hgignore")
    if (-f ".hgignore");

# Write the new one
open(FILE, ">.hgignore");
print FILE join("\n", @hgignore) . "\n";
print FILE join("\n", @globals) . "\n"; 
close(FILE);
print "All done!\n";
exit(0);

#######################################################################

# DFS-oriented recursive directory search
sub process {
    my $dir = shift;

    # Look at the svn:ignore property for this directory
    my $svn_ignore = `svn pg svn:ignore $dir 2> /dev/null`;
    # If svn failed, bail on this directory.
    return
        if ($? != 0);

    chomp($svn_ignore);
    if ($svn_ignore ne "") {
        print "Found svn:ignore in $dir\n"
            if ($debug);
        foreach my $line (split(/\n/, $svn_ignore)) {
            chomp($line);
            $line =~ s/^\.\///;
            next
                if ($line eq "");

            # Ensure not to ignore special hg files
            next
                if ($line eq ".hgignore" || $line eq ".hgrc" || 
                    $line eq ".hg");
            # We're globally ignoring some specials already; we can
            # skip those
            my $skip = 0;
            foreach my $g (@globals) {
                if ($g eq $line) {
                    $skip = 1;
                    last;
                }
            }
            next 
                if ($skip);

            push(@hgignore, "$dir/$line");
        }
    }
        
    # Now find subdirectories in this directory
    my @entries;
    opendir(DIR, $dir) || die "Cannot open directory \"$dir\" for reading: $!";
    @entries = readdir(DIR);
    closedir DIR;

    foreach my $e (@entries) {
        # Skip special directories and sym links
        next
            if ($e eq "." || $e eq ".." || $e eq ".svn" || $e eq ".hg" ||
                -l "$dir/$e");

        # If it's a directory, analyze it
        process("$dir/$e")
            if (-d "$dir/$e");
    }
}
