#!/usr/bin/perl
# Wendet glas-patches.txt auf einen Kodi-Quellbaum an (Vero: apply-glas.sh; Windows liest dieselbe Datei in
# windows/apply-glas.ps1). Aufruf im Quellbaum: perl glas-patches.pl /pfad/glas-patches.txt
# Bricht ab, wenn ein Anker fehlt; Bloecke, deren Einsatz schon in der Datei steht, werden uebersprungen.
use strict;
use warnings;

my $spec = shift or die "Aufruf: glas-patches.pl <glas-patches.txt>\n";
open(my $fh, '<', $spec) or die "$spec: $!\n";
local $/;
my $text = <$fh>;
close($fh);
$text =~ s/\r\n/\n/g;

my @lines = split /\n/, $text, -1;
my ($mode, $file, $what, $part, @anchor, @insert);
my ($applied, $skipped) = (0, 0);
for my $line (@lines) {
    if (!defined $mode) {
        if ($line =~ /^\@\@ (after|before|replace) (\S+) \| (.+)$/) {
            ($mode, $file, $what, $part) = ($1, $2, $3, 'anchor');
            @anchor = ();
            @insert = ();
        }
        next;
    }
    if ($line eq '@@--') {
        $part = 'insert';
        next;
    }
    if ($line eq '@@end') {
        my $a = join('', map { "$_\n" } @anchor);
        my $b = join('', map { "$_\n" } @insert);
        open(my $in, '<', $file) or die "$file: $!\n";
        my $src = <$in>;
        close($in);
        if (index($src, $b) >= 0) {
            $skipped++;
        } else {
            my $i = index($src, $a);
            die "Anker fehlt ($what): $anchor[0]\n" if $i < 0;
            warn "Anker mehrfach vorhanden ($what), erste Fundstelle genommen\n" if index($src, $a, $i + 1) >= 0;
            if ($mode eq 'after') {
                substr($src, $i + length($a), 0) = $b;
            } elsif ($mode eq 'before') {
                substr($src, $i, 0) = $b;
            } else {
                substr($src, $i, length($a)) = $b;
            }
            open(my $out, '>', $file) or die "$file: $!\n";
            print $out $src;
            close($out);
            $applied++;
        }
        undef $mode;
        next;
    }
    if ($part eq 'anchor') {
        push @anchor, $line;
    } else {
        push @insert, $line;
    }
}
die "Block ohne \@\@end ($what)\n" if defined $mode;
print "glas-patches: $applied angewendet, $skipped schon vorhanden\n";
