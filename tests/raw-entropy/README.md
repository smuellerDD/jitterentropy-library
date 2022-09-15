# Jitter RNG SP800-90B Entropy Analysis Tool

This archive contains the SP800-90B analysis tool to be used for the
Jitter RNG.

See the README files in the different sub directories.

# SP 800-90B and Entropy Estimates

Statistical entropy estimates are performed using NIST's `ea_noniid` tool.

The Jitter RNG uses a parameter called `osr` as the basis for its
internal entropy estimate for each raw noise sample. This parameter
is set when allocating the jitter entropy context using the
`jent_entropy_collector_alloc` call.

The Jitter RNG internally presumes that the raw noise contains

$$H_{\text{min}} \geq \frac{1}{\texttt{osr}}$$

per raw noise sample ("time delta"). This implies that the statistical
entropy estimate (`H_original` in SP 800-90B terminology) must be at
least this value.  In more formal validation settings, it is necessary
to separately estimate a heuristic lower bound for the entropy.

# Approach to Justify a Particular Heuristic Entropy Estimate

In general, it is difficult to produce a stochastic model for non-physical
noise sources that applies across different hardware. In this section,
we present an approach for generating a technical (heuristic) argument
for why this noise source can support a stated entropy rate on particular
hardware.

## General Approach
One can characterize an IID noise source using only an adequately detailed
histogram, as the min entropy of such a source is established by the
symbol probabilities viewed in isolation. For a non-IID noise source,
this yields only an upper bound for the min entropy. This occurs because
non-IID sources have statistical memory: that is, there is internal
state that induces relationships between the current output and some
number of past outputs.  The statistical memory “depth” is the number
of symbols for which that state induces a significant interrelationship.

If the impact of past outputs is significant in future outputs, then
any histogram is possible while simultaneously having vanishingly small
entropy levels. Similarly, the SP 800-90B entropy estimators are often
capable of producing meaningful min entropy bounds for single reasonably
stable distributions, but they are likely to (sometimes dramatically)
overestimate the available min entropy when they are provided data from a
progression of distinct distributions, even when these sub-distributions
are sampled in a completely deterministic fashion.

This approach attempts to address these assessment challenges.
The approach here has essentially four steps.
1. Use the `analyze_memsize.sh` script to generate (non-decimated)
data samples for a wide variety of memory sizes.
2. Create and examine raw data histograms for noise source outputs across
the tested candidate memory settings in order to select the size of the
memory area used by the primary noise source (`jent_memaccess`). Each
setting is selected using the `JENT_MEMORY_SIZE_EXP` macro.  For each
memory setting, the submitter should perform an initial review of the
resulting symbol histograms. It is likely that using a larger memory
region will significantly affect the observed distributions, as a larger
memory region leads to more cache misses and/or a different mix of hits
for the various cache levels. This progression continues until the
distribution becomes essentially fixed at a *terminal distribution*,
whence additionally increasing the memory size has limited observable
impact on the resulting histogram.  On most architectures, the delays
associated with updates that resolve purely within the cache system are
both more predictable and have significantly lower variation as compared
to the same updates that resolve in RAM reads and writes, so it is useful
to set the memory size to at least the smallest value that attains this
*terminal distribution*. Testing thus far suggests that this terminal
distribution is generally attained before the memory size of the the
`jent_memaccess` buffer is 8 times the cache size.
3. Select a single sub-distribution of interest.  This should
be a sub-distribution that is both common (ideally the selected
sub-distribution would include over 80% of the observed values)
and suitably broad to support a reasonable entropy level.  This
sub-distribution is provided while setting the `JENT_DISTRIBUTION_MIN` and
`JENT_DISTRIBUTION_MAX` macros.
4. Configure the `analyze_depth.sh` tool by inputting the selected
`JENT_MEMORY_SIZE_EXP`, `JENT_DISTRIBUTION_MIN` and `JENT_DISTRIBUTION_MAX`
settings, and run this script to help estimate the statistical memory
depth of the system.  The timing data is probabilistically decimated
at a rate governed by the `JENT_MEMORY_DEPTH_EXP` parameter.  If a
tested `JENT_MEMORY_DEPTH_EXP` setting results in the produced data set
passing the NIST SP 800-90B Section 5 IID tests at a suitably high rate,
then a histogram-based entropy estimate can be applied as the heuristic
entropy estimate.

A passing combination of the `JENT_MEMORY_SIZE_EXP`, `JENT_DISTRIBUTION_MIN`,
`JENT_DISTRIBUTION_MAX`, and `JENT_MEMORY_DEPTH_EXP` should then be
used for the library.

## Statistically Meaningful IID Testing
A single test result from the NIST SP 800-90B IID tests is not meaningful
for this testing, but it would be reasonable to instead test many data
sets in this way. For each of the 22 IID tests (e.g., Excursion Test
Statistic, Chi-Square Independence test, etc.), the result of repeated
testing using disjoint data subsets can be viewed as “passing”
so long as the proportion of that test passing is larger than some
pre-determined cutoff.

In the SP 800-90B Section 5 IID tests, each IID test is designed to have
a false reject rate of

$$p_{\text{per-test false reject}} = \frac{1}{1000} \text{.}$$

One can calculate the (one-sided) p-value for the number of observed
test failures using the binomial distribution: if we denote the number
of observed failures as `k` and the CDF of the failure count Binomial
Distribution (with parameters

$$p=\frac{1}{1000}$$

and `n` being the number of 1 million sample non-overlapping data sets)
as `F(x)` , then we can calculate the p-value as

$$p_{\text{value}} = 1-F \left( k-1 \right) \text{.}$$

SP 800-90B uses a significance of 1%, so for this testing, we want to
select a test failure cutoff so that the overall round (that is, all
22 tests) has a 99% chance of passing when the null hypothesis is true
(that is, when the source is actually IID). For this to occur, all 22 of
the IID tests need to pass, and under the null hypothesis, these tests
are independent.  If we call the targeted per-test chance of failure
`q`, then we can bound the chance of observing one or more failures in
the the 22 independent SP 800-90B Section 5 IID tests as

$$ 1 - \left( 1 - q \right)^{22} \leq 0.01 $$

or equivalently

$$ q \leq 1 - 0.99^{1/22} \approx 0.000456729 \text{.}$$

For this per-test cutoff and 147 tests, we find that we can tolerate up to
3 failures on a per-test basis. (Indeed, 147 tests is the smallest number
of tests that can tolerate 3 failures in this argument) In this case, if
4 or more failures are observed for any specific IID test then we conclude
that the source is non-IID (that is, we reject the null hypothesis with
a 1% significance). Similarly, if all tests have 3 or fewer failures,
then the testing supports the hypothesis that the noise source is IID
(and thus, we have decimated sufficiently).

In order for this approach to be meaningful, this testing would have to
show the following properties:

* Property A: The IID testing must fail badly for the non-decimated data set,
indicating that the SP 800-90B tests are sensitive to a style of defect
present in the system. If the original data does not fail this testing,
then we cannot use these tests to estimate the memory depth.
* Property B: The IID test results must generally improve as the decimation rate
increases (i.e., the proportion of observed “passes” should generally
increase).
* Property C: All of the IID tests must eventually “pass” at a rate
consistent with the cutoff developed above for some specific selection of
`JENT_MEMORY_DEPTH_EXP`.

## Worked Example
### Test System
The following tests were conducted using a system with a Intel Xeon
6252 CPU (36MB cache) and 384 GB memory.

### Counter Source
On x86-64 platforms, the `rdtscp` (“Read Time-Stamp Counter”) instruction
provides a clock that runs at some CPU-defined clock rate. From ["Intel
64 and IA-32 Architectures Software Developer’s Manual, Volume 3",
Section 17.17]:

> The time-stamp counter ... is a 64-bit counter that is set to 0
> following a RESET of the processor. Following a RESET, the counter
> increments even when the processor is halted...
>
> Processor families increment the time-stamp counter differently:
> [(Older) Option 1:] the time-stamp counter increments with every internal processor clock cycle.
>
> [(Newer) Option 2:] the time-stamp counter increments at a constant
> rate... Constant TSC behavior ensures that the duration of each clock
> tick is uniform and supports the use of the TSC as a wall clock timer
> even if the processor core changes frequency. This is the architectural
> behavior moving forward…
>
> The time stamp counter in newer processors may support an enhancement,
> referred to as invariant TSC... This is the architectural behavior moving
> forward. On processors with invariant TSC support, the OS may use the
> TSC for wall clock timer services...  The features supported can be
> identified by using the CPUID instruction.

With Linux systems, the “invariant TSC” CPU feature is available when
both the `constant_tsc` and `nonstop_tsc` CPU feature flags are
present in /proc/cpuinfo.

On this platform, the used counter is the TSC, so the counter is
sufficiently fine-grained to support `JENT_MEMACCESSLOOP_EXP = 0`. On
a platform where the observed distribution is very narrow we may have
to increase this parameter. Note: increasing this parameter too much
will lead to nearly full entropy samples, which will result in the system
not fulfilling Property A.  It is useful to use the smallest value of
`JENT_MEMACCESSLOOP_EXP` that yields a reasonable distribution.

### Test Results
For step #1, the `analyze_memsize.sh` script was used to generate
(non-decimated) data for settings between 10 and 30.

For Step #2, the following histograms were generated:

![Distributions Across Memory Sizes](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/distanim.gif)

Note: These histograms (and all following diagrams and specified values)
are with respect to the number of distinct increments of the counter. In
the tested architecture, the counter is incremented in multiples of 2,
so all values used by JEnt are divided by this common factor.

We see here that the memory read/update event mostly results in actual
RAM reads when `JENT_MEMORY_SIZE_EXP` is set to 27 or larger; the last two
histograms show essentially the same behavior. As such, the terminal
distribution occurs when `JENT_MEMORY_SIZE_EXP` is 28 or higher.

For this evaluation, we proceed with the `JENT_MEMORY_SIZE_EXP` setting of 28
(resulting in a memory region of 256 MB). This results in the following histogram
for non-decimated data:

![IID Testing Results](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/non-dec-hist-28.svg)

In this diagram, we see a couple of spikes in the [50, 100] interval,
associated data being cached in the L3 cache. We are interested in
results that result in RAM IO, so we are interested in the next two
(dominant) spikes in the interval [105, 185]. As such, for Step 3,
the distribution that we are interested in is in the interval [105, 185].

For Step 4, we used the `analyze_depth.sh` script to generate 147 million
samples for each of the tested `JENT_MEMORY_DEPTH_EXP` settings, using
the parameters `JENT_MEMORY_SIZE_EXP = 28`, `JENT_DISTRIBUTION_MIN = 105`,
and `JENT_DISTRIBUTION_MAX = 185`.  We then performed IID testing on each
of the 147 1-million sample subsets for each `JENT_MEMORY_DEPTH_EXP`
setting. The IID testing results were as follows:

![IID Testing Results](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/IID-testing.svg)

Here, a "round" is a full SP 800-90B IID test round conducted on one of
the disjoint data subsets of 1-million samples.  A round is considered
to be passing if and only if all 22 of the IID tests pass for that
particular subset. The "Tests Passing" refers to the total proportion
of the IID tests that pass across all 147 tested subsets for each tested
memory depth setting.

This shows that the data seems to become increasingly close to IID
behavior as the `JENT_MEMORY_DEPTH_EXP` value is increased.  Note that
this distribution family seems to satisfy all of the properties described
above:

* Property A: For `JENT_MEMORY_DEPTH_EXP = 0`, 100% of the 147 distinct
rounds fail the SP 800-90B Section 5 IID testing.
* Property B: It is clear from the "Tests Passing" proportion that more
tests tend to pass as `JENT_MEMORY_DEPTH_EXP` is increased.
* Property C: The required number of each test pass when
`JENT_MEMORY_DEPTH_EXP = 11`.

We note that the data passes under the above interpretation for
`JENT_MEMORY_DEPTH_EXP = 11`.

Using the parameters `JENT_MEMORY_DEPTH_EXP = 11`, `JENT_MEMORY_SIZE_EXP =
28`, `JENT_DISTRIBUTION_MIN = 105`, and `JENT_DISTRIBUTION_MAX = 185`,
the source produces the following histogram.

![IID Testing Results](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/final-hist.svg)

Now that the source is behaving as an IID source, we can directly produce
an estimate for the entropy, namely approximately

$$H_{\text{submitter}} = - \log_2 ( 0.0518297 ) \approx 4.27$$

bits of min entropy per symbol.

The results from the SP 800-90B non-IID statistical estimation track
(`ea_noniid`, when using two verbose flags) is as follows:

```
[...]
H_bitstring = 0.50729119109721543
H_bitstring Per Symbol = 3.5510383376805081
H_original = 4.1991141990845975
Assessed min entropy: 3.5510383376805081
```

The last value gives you an upper bound for the min entropy per time
delta, namely
$$H_{\text{original}} \approx 4.2 \text{ and } n \times H_{\text{bitstring}} \approx 3.5 \text{.}$$

We then have a min entropy estimate of

$$ H_I = \text{min} \left( H_{\text{original}},  n \times H_{\text{bitstring}}, H_{\text{submitter}} \right) \approx 3.5 $$

bits of min entropy per symbol.

In the example above, the measurement shows that as much as 3.5 bits of
entropy can be credited which implies that the available amount of entropy
may be more than what the Jitter RNG heuristic applies even when setting
`osr` to the smallest possible value (`osr = 1`).

## Commentary

The decimation has a significant impact on the rate that the noise source
outputs data, and this has an impact on the rate that the library can
output conditioned data. On the test system with `JENT_MEMORY_DEPTH_EXP
= 0` (no decimation), this entropy source produces approximately 287
conditioned outputs every second (where each output is 256 bits). On
average, the probabilistic decimation outputs a value from the noise
source approximately every

$$3 \times 2^{\text{JENT\\_MEMORY\\_DEPTH\\_EXP} - 1} - \frac{1}{2}$$

candidates, so the setting `JENT_MEMORY_DEPTH_EXP = 11` reduces the
output rate from the noise source by a factor of approximately 3071.5. As
it turns out, the conditioning time dwarfs the memory update time, so
this results in a rate of approximately 1.5 outputs per second (which is
considerably faster than if the noise source output rate was the determining
factor.)

With this degree of slowdown, is the use of this option "worth it"?  First,
it is important to point out that even though `JENT_MEMORY_DEPTH_EXP`
may look like it is "throwing away" significant amounts of (possibly
entropy-containing) data, the decimated values are fed into the
conditioner as "supplemental data". As such, though no entropy can be
formally claimed associated with this data, in practice any entropy
would be retained within the conditioning function.  As such, use of
this value is expected to only make the security of the system better,
as considerably more data is sent into the conditioning function.

If we look at the statistical assessment of the non-decimated data,
we find the following:

```
[...]
H_bitstring = 0.48854235733379042
H_bitstring Per Symbol = 3.419796501336533
H_original = 3.8331319722313415
Assessed min entropy: 3.419796501336533
```

It is useful to note that, while the decimated data and
non-decimated data seem to perform essentially the same way in the SP
800-90B non-IID entropy estimators, IID testing indicates that there is
a significant difference between the decimated and non-decimated data
streams. This suggests that the non-decimated data suffers from some
statistical flaws that are not detected (and thus not accounted for) by
the non-IID entropy estimators. This suggests that decimation helps
prevent over crediting entropy in the system, and thus there is a technical
reason for including the decimation beyond production of a defensible
heuristic entropy estimate.

In the case where one is unconcerned with a formal validation or the
abstract risk of over-estimating the produced entropy, it is probably
reasonable to set `JENT_MEMORY_DEPTH_EXP = 0`. Otherwise, it is best to
select an architecture-specific value supported by the described testing.

# Authors
Stephan Mueller <smueller@chronox.de>
Joshua E. Hill <josh@keypair.us>
