#
# R script for visualizing the entropy rates from the memory access
#

# Load the file created by processdata_hashloop.sh
args <- commandArgs(trailingOnly = TRUE)

if (length(args) != 2) {
	stop("Invoke with <heading> <input file>")
}

main <- args[1]
src <- args[2]
out <- args[2]

deterministic <- scan(src, sep=",", nlines=1, what=numeric())
min_deterministic <- scan(src, sep=",", nlines=1, skip=1, what=numeric())
min_pairs_deterministic <- scan(src, sep=",", nlines=1, skip=2, what=numeric())
min_triple_deterministic <- scan(src, sep=",", nlines=1, skip=3, what=numeric())

x <- c(1:length(deterministic))
coln <- c("90B Entropy", "Min Entropy", "Min Entropy Pairs", "Min Entropy Triplets")

# update if JENT_HASH_LOOP_INIT is modified
rown <- c("3", "6", "12", "24", "48", "96", "192", "384")

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

plot(x, deterministic, xlim=c(1,8), ylim=c(0, 8),
	xaxt="n",
	main=main,
	xlab="Hash Loop Iteration Count",
	ylab="Entropy Rate [bits/time delta]", type="b")

lines(x, deterministic, type = "b", col = "red")
lines(x, min_deterministic, type = "b", col = "blue")
lines(x, min_pairs_deterministic, type = "b", col = "cyan")
lines(x, min_triple_deterministic, type = "b", col = "orange")

axis(1, at=x, labels=rown)

legend("topleft", legend = c(coln),
       col = c("red", "blue", "cyan", "orange"),
       lty = 1)

dev.off()
