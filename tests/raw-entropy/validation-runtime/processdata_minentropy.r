# Calculation of the lower boundary of the min entropy over array p
# H = -log2(max(p))
#
min.entropy <- function(p)
{
	p.norm <- table(p)/length(p)
	-log2(max(p.norm))
}

args <- commandArgs(trailingOnly = TRUE)
src <- args[1]

filedata <- scan(file=src, what=numeric())

#
# Only take the low 8 bits (i.e. modulo 256) to make the
# min entropy data comparable to the SP800-90B data which
# also takes only the low 8 bits
#
filedata <- filedata %% 256

# Gather all odd and even entries
odd_filedata <- filedata[c(TRUE,FALSE)]
even_filedata <- filedata[c(FALSE,TRUE)]
# Now calculate the pairs by simply "concatenating" odd with the even data. As R cannot really concatenate, we multiply the odd by 1 mil (an arbitrary value to guarantee be larger than the largest value in file data) and add the even entries
pair_filedata <- odd_filedata * 1000000 + even_filedata

# Gather all triplets
first_filedata <- filedata[c(TRUE,FALSE,FALSE)]
second_filedata <- filedata[c(FALSE,TRUE,FALSE)]
third_filedata <- filedata[c(FALSE,FALSE,TRUE)]
# Now calculate the pairs by simply "concatenating" the data. As R cannot really concatenate, we multiply the third by 10,000 * 10,000 and the second by 10,000 (an arbitrary value to guarantee be larger than the largest value in file data) and add the first entries
triplet_filedata <- (third_filedata * 10000 * 10000) + (second_filedata * 10000) + first_filedata

round(min.entropy(filedata), 3)
round(min.entropy(pair_filedata), 3) / 2
round(min.entropy(triplet_filedata), 3) / 3
