#
# R script for visualizing the entropy rates from the memory access
#

# Data to be modified: provide the cache size here in bytes
L1 <- 48 * 1024
L2 <- 2 * 1024 * 1024
L3 <- 24 * 1024 * 1024

# Load the file created by processdata_memloop.sh
args <- commandArgs(trailingOnly = TRUE)

if (length(args) != 1) {
	stop("Invoke with <input file>")
}

src <- args[1]
out <- args[1]

deterministic <- scan(src, sep=",", nlines=1, what=numeric())
min_deterministic <- scan(src, sep=",", nlines=1, skip=1, what=numeric())
min_pairs_deterministic <- scan(src, sep=",", nlines=1, skip=2, what=numeric())
min_triple_deterministic <- scan(src, sep=",", nlines=1, skip=3, what=numeric())

x <- c(1:length(deterministic))
coln <- c("Deterministic Memory Access (90B Entropy)", "Deterministic Memory Access (Min Entropy)", "Deterministic Memory Access (Min Entropy Pairs)", "Deterministic Memory Access (Min Entropy Triplets)")
rown <- c("1kB", "2kB", "4kB", "8kB", "16kB", "32kB", "64kB", "128kB", "256kB", "512kB",
	  "1MB", "2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB", "256MB", "512MB")

# Print out the matrix of data
rawdata <- matrix(c(deterministic, min_deterministic, min_pairs_deterministic, min_triple_deterministic), ncol=4,
		  dimnames = list(rown, coln))
filename <- sprintf("%s.txt", out)
sink(filename)
print(rawdata)
sink()

# Draw the matrix of data
filename <- sprintf("%s.pdf", out)
pdf(filename, width=8, height=5, pointsize=10)

plot(x, deterministic, xlim=c(1,20), ylim=c(0, 8),
	xaxt="n",
	main='Memory Access Entropy Rate',
	xlab="Memory Size",
	ylab="Entropy Rate [bits/time delta]", type="b")

lines(x, deterministic, type = "b", col = "red")
lines(x, min_deterministic, type = "b", col = "blue")
lines(x, min_pairs_deterministic, type = "b", col = "cyan")
lines(x, min_triple_deterministic, type = "b", col = "orange")

axis(1, at=x, labels=rown)

# Calculate the offsets for the caches: log2(cachesize) - 9
# because 1kB starts at x-Axis value 1
abline(v=log2(L1) - 9, col = "green", lwd = 1)
# L2 boundary = L2 + L1
abline(v=log2(L2 + L1) - 9, col = "green", lwd = 1)
# L3 boundary = L3 + L2 + L1
abline(v=log2(L3 + L2 + L1) - 9, col = "green", lwd = 1)

legend("topleft", legend = c(coln, "L1 / L2 / L3 Cache Boundaries"),
       col = c("red", "blue", "cyan", "orange", "green"),
       lty = 1)

dev.off()
