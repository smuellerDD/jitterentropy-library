#!/usr/bin/perl

my $data1=-1.0;
my $dataCount = 0;
print "{";
while ( <> ) {
	if(/^Non-Binary Chi-Square Independence: p-value = ([0-9.eE-]+)$/) {
		$data1 = $1;
	} elsif(/^Non-Binary Chi-Square Goodness-of-Fit: p-value = ([0-9.eE-]+)$/) {
		die unless $data1 >= 0;
		if( $dataCount > 0 ) {
			print ", ";
		}
		print "{$data1, $1}";
		$data1 = -1.0;
		$dataCount ++;
	}
}
	print "};\n";
