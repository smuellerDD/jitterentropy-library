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
least this value. In more formal validation settings, it is necessary
to separately estimate a heuristic lower bound for the entropy.

# Analysis Approaches to Justify a Particular Heuristic Entropy Estimate

In general, it is difficult to produce a stochastic model for non-physical
noise sources, and if such a model were constructed it would apply only
to a very specific instantiating of the non-physical source.

Software noise sources that are based on variations of system timings
commonly display timing distributions that are markedly multi-modal;
these modes can be broadly grouped into sub-distributions. In general,
the sub-distributions reflect timing variation in some fixed set of
events. Distinct sub-distributions reflect distinct successions of events
that lead to a particular delay. Some of these successions of events
emerge as being quite common, and some reflect sporadic behavior that is
so uncommon that it is difficult to characterize. This latter sporadic
behavior has no substantive impact on the bulk behavior of the noise
source, so we can restrict ourselves to examining the sub-distributions
that are associated with reasonably common sub-distributions.

In this section, we first summarize the analysis approach implemented in the
original JEnt library testing scripts. We then present a procedure that
is shared between both of the new analysis approaches. Finally, we present two
new analysis approaches for generating a technical heuristic argument for why this
noise source can support a stated entropy rate on particular hardware.

## The *Original JEnt* Analysis Approach
The original JEnt library attempts to allocate a power-of-two-sized memory
region for the `jent_memaccess` source that is larger than the total
amount of cache found. This makes it likely that at least some updates
resolve to RAM I/O, but the proportion of these that do so is expected to
be no more than 50% of the requests. There is no built-in mechanism
for requesting a larger memory region that works across architectures,
but there are two ways of specifying an upper bound for the initial size of this
memory region. In the event of self-test failure, this parameter may be
increased beyond the specified `jent_memaccess` memory region size bound.

The original JEnt library uses the full timing of the memory update and
conditioning computation as the raw data. The variation in this raw
data does depend on the memory update timing (the variation of which
will largely depend on cache timing) but it is predominately
driven by the variability of the conditioning function timing.
This variability largely depends on other architectural elements (branch
prediction, pipelining, etc.) and operating system characteristics (scheduling,
interrupts, etc.)

The scale of the resulting timing data is generally much larger than
can be represented in an 8-bit value, so the data must be mapped to
an 8-bit quantity. Documentation suggests mapping by taking the lower
8-bits of the 64-bit timing data, but other mapping methodologies are
possible. Any mapping methodology essentially superimposes all of the
occurring sub-distributions in a way established by the mapping.
The resulting mapped raw data is analyzed by the SP 800-90B non-IID
track, and the resulting value is used as both `H_submitter` and
`H_original` in the SP 800-90B evaluation process.

### Required Assumptions
The Original JEnt Analysis Approach relies on a few baseline assumptions:
1. All variation that meaningfully changes the result of the SP 800-90B entropy estimators is a consequence of non-determinism within the system and not the result of a complicated deterministic system.
2. The complete set of SP 800-90B estimators (i.e., the non-IID track result) provides a reasonable lower bound for the min entropy produced by the noise source. In particular:
	- The observed multi-sub-distribution data does not cause the SP 800-90B non-IID track to output an artificially high min entropy estimate.
	- The mapping methodology does not cause the SP 800-90B non-IID track to output an artificially high min entropy estimate.
	- The entropy estimate reflects the behavior of the "worst case" sub-distribution that an attacker can cause to become dominant.

To verify the first assumption, there is no statistical test that
can distinguish between complicated deterministic behavior and
non-deterministic behavior, so some design-based argument is necessary.

For the second assumption, characterizing a sub-distribution's min entropy
always requires some knowledge of how the non-determinism present in
the system manifests. Similarly, there must be some understanding of
what deterministic variation is expected.

We need to be able to argue that the entropy estimator producing the
non-IID track's result provides a reasonable min entropy lower bound
for the noise source and does not over-credit the min entropy due to
variation that is essentially deterministic in its origin.

The estimators vary in complexity and response to the data, so it isn't
clear how to demonstrate that the estimators provide a reasonable lower
bound for the source min entropy.


## Noise Source Changes to Help Ease Analysis
The SP 800-90B entropy estimators are often capable of producing
meaningful min entropy bounds for single reasonably stable distributions,
but they are likely to (sometimes dramatically) overestimate the available
min entropy when they are provided data from a progression of distinct
sub-distributions, even when the selection of the sub-distributions
is made in a completely deterministic fashion. If a noise source
essentially behaves in one of `n` distinct ways, then it is commonly
more meaningful to assess each of the possible `n` sub-distributions,
as the *overall* assessment of all the data (inclusive of all the
noted sub-distributions) may assess considerably higher than any of the
sub-distributions. Additionally, it is commonly possible for an active
attacker to affect the relative proportion in which each of the observed
sub-distributions occur, and so can affect the amount of uncertainty
that the system can produce.

Both of the analysis approaches described here attempt to make it more
likely that the SP 800-90B estimators produce meaningful results by
narrowing the scope of the entropy estimation to a specific observed
sub-distribution. This occurs using the following steps:
1. Use the `analyze_memsize.sh` script to generate (non-decimated)
data samples for a wide variety of memory sizes.
2. Create and examine raw data histograms for noise source outputs across
the tested candidate memory settings in order to select the size of the
memory area used by the primary noise source (`jent_memaccess`). Each
setting is selected using the `JENT_MEMORY_SIZE_EXP` macro. For each
memory setting, the submitter should perform an initial review of the
resulting symbol histograms. It is likely that using a larger memory
region will significantly affect the observed distributions, as a larger
memory region leads to more cache misses and/or a different mix of hits
for the various cache levels. This progression continues until the
distribution becomes essentially fixed at a *terminal distribution*,
whence additionally increasing the memory size has limited observable
impact on the resulting histogram. On most architectures, the delays
associated with updates that resolve purely within the cache system are
both more predictable and have significantly lower variation as compared
to the same updates that resolve in RAM reads and writes, so it is useful
to set the memory size to at least the smallest value that attains this
*terminal distribution*. Testing thus far suggests that this terminal
distribution is generally attained before the memory size of the
`jent_memaccess` buffer is 8 times the cache size.
3. Select a single sub-distribution of interest. This should
be a sub-distribution that is both common (ideally the selected
sub-distribution would include over 80% of the observed values; if the
selected sub-distribution includes less than 10% of the observed values,
then the distribution health test must be adjusted) and suitably broad to
support a reasonable entropy level. This sub-distribution is provided
by setting the `JENT_DISTRIBUTION_MIN` and `JENT_DISTRIBUTION_MAX`
macros. This identified sub-distribution should be the result of some
readily identifiable architectural characteristic of the underlying
hardware that is thought to be non-deterministic.

The result of this configuration is that the noise source outputs only
values within the expected sub-distribution range, so only this reduced
data set needs to be able to be reasonably assessed. In many instances the
data from the selected sub-distribution can be used without translation,
so there is no risk that translation masks patterns from statistical
assessment.

This makes any use of min entropy estimators more meaningful, and further
should make the included health testing more statistically powerful.

## The *Essentially IID* Analysis Approach
### Procedure
One can characterize an IID noise source using only an adequately detailed
histogram, as the min entropy of such a source is established by the
symbol probabilities viewed in isolation. For a non-IID noise source,
this yields only an upper bound for the min entropy. This occurs because
non-IID sources have statistical memory: that is, there is internal
state that induces relationships between the current output and some
number of past outputs. The statistical memory “depth” is the number
of symbols for which that state induces a significant interrelationship.

If the impact of past outputs is significant in future outputs, then
any histogram is possible while simultaneously having vanishingly small
entropy levels.

In addition to the three steps defined for Selection of a Sub-Distribution, perform the following step:

- **Step 4**: Configure the `analyze_depth.sh` tool by inputting the
selected `JENT_MEMORY_SIZE_EXP`, `JENT_DISTRIBUTION_MIN` and
`JENT_DISTRIBUTION_MAX` settings, and run this script to help
estimate the statistical memory depth of the system. The timing
data is probabilistically decimated at a rate governed by the
`JENT_MEMORY_DEPTH_EXP` parameter. If a tested `JENT_MEMORY_DEPTH_EXP`
setting results in the produced data set passing the NIST SP 800-90B
Section 5 IID tests at a suitably high rate, the noise source is
*essentially IID* when using that parameter set, and a simple Most Common
Value (MCV) min entropy estimate (which is essentially histogram-based)
can be used as the heuristic entropy estimate.

A passing combination of the `JENT_MEMORY_SIZE_EXP`, `JENT_DISTRIBUTION_MIN`,
`JENT_DISTRIBUTION_MAX`, and `JENT_MEMORY_DEPTH_EXP` should be
used to configure the library.

As a formal matter within the SP 800-90B evaluation, we won't make an
IID claim; with parameters as selected above, the system empirically behaves
in a way that is consistent with an IID system, but it is difficult to
justify why the hardware design yields this result without a thorough
and design-specific review of the hardware implementation.

### Statistically Meaningful Large Scale IID Testing
A single test result from the NIST SP 800-90B IID tests is not meaningful
for this testing, but a reasonable statistical test can be constructed
by testing many data sets in this way. For each of the 22 IID tests
(e.g., Excursion Test Statistic, Chi-Square Independence test, etc.),
the result of repeated testing using disjoint data subsets can be viewed
as “passing” so long as the proportion of that test passing is larger
than some pre-determined cutoff.

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

SP 800-90B uses a significance of 1%, so for this testing we want to
select a test failure cutoff so that the overall round (all
22 tests) has a 99% chance of passing when the null hypothesis is true
(that is, when the source is actually IID). For this to occur, all 22 of
the IID tests need to pass, and under the null hypothesis, these tests
are independent. If we call the targeted per-test chance of failure
`q`, then we can bound the chance of observing one or more failures in
the 22 independent SP 800-90B Section 5 IID tests as

$$ 1 - \left( 1 - q \right)^{22} \leq 0.01 $$

or equivalently

$$ q \leq 1 - 0.99^{1/22} \approx 0.000456729 \text{.}$$

For this per-test cutoff and 147 tests, we find that we can tolerate up to
3 failures on a per-test basis (and 147 tests is the smallest number
of tests that can tolerate 3 failures in this argument.) In this case, if
4 or more failures are observed for any specific IID test then we conclude
that the source is non-IID (that is, we reject the null hypothesis with
a 1% significance). Similarly, if all tests have 3 or fewer failures,
then the testing supports the hypothesis that the noise source is IID,
and thus, the noise source has been decimated sufficiently.

### Required Assumptions
The Essentially IID Analysis Approach relies on the following assumptions:
1. The probability of the most common symbol is a established by the underlying non-determinism within the system.
2. The SP 800-90B IID tests are a reasonable way of determining if we have sufficiently decimated the data to the point where the source is *essentially IID*.

Characterizing a sub-distribution's min entropy always requires
some knowledge of how the non-determinism present in the system
manifests. Similarly, there must be some understanding of what
deterministic variation is expected. To verify the first assumption,
there is no statistical test that can distinguish between complicated
deterministic behavior and non-deterministic behavior, so some design-based
argument is necessary. It is important to note that not all hardware
experiences this sort of variation, and indeed some types of hardware are
engineered so that their timing is both wholly deterministic and regular.

Under **Assumption 1** and **Assumption 2**, once the source assesses as
*essentially IID*, then a simple Most Common Value (MCV) will produce
a meaningful min entropy estimate that can be used to generate the heuristic
min entropy estimate.

One way to support **Assumption 2**, would be to demonstrate the following
properties:

- Property A: The IID testing must fail for the non-decimated data set,
indicating that the SP 800-90B tests are sensitive to a style of defect
present in the system. If the original data does not fail this testing,
then we cannot use these tests to estimate the memory depth.
- Property B: All of the IID tests must eventually “pass” at a rate
consistent with the cutoff developed above for some specific selection of
`JENT_MEMORY_DEPTH_EXP`.

If the IID testing passes with no decimation, then it is difficult
to distinguish between the noise source being essentially IID without
decimation and the noise source having undetected defects.

If no tested `JENT_MEMORY_DEPTH_EXP` results in a passing IID claim (or if
the `JENT_MEMORY_DEPTH_EXP` parameter that results in a pass yields a noise
source that is too inefficient), then the assumptions underlying the
*Essentially IID* Analysis Approach need to be justified in some other way.

## The *Single Sub-Distribution Empirical* Analysis Approach
### Procedure
There is generally no requirement that a noise source be IID.
Further, tuning noise source parameters so that the noise source becomes essentially
IID often makes that noise source rather inefficient.

In the Single Sub-Distribution Empirical Analysis Approach, we use a stronger assumption set
that is anticipated to apply to fewer noise sources, but which yields
a much more efficient noise source.

In this analysis approach, a combination of the `JENT_MEMORY_SIZE_EXP`,
`JENT_DISTRIBUTION_MIN`, and `JENT_DISTRIBUTION_MAX` parameters with
the desired values (see above) should be used to configure the
library. `JENT_MEMORY_DEPTH_EXP` can be freely set (the value 0 would
be acceptable in this analysis approach, and is the most efficient setting.)

### Required Assumptions
The Single Sub-Distribution Empirical Analysis Approach relies on a few baseline assumptions:
1. All variation that meaningfully changes the result of the SP 800-90B entropy estimators is a consequence of non-determinism within the system and not the result of a complicated deterministic system.
2. The complete set of SP 800-90B estimators (i.e., the non-IID track result) provides a reasonable lower bound for the min entropy produced by the noise source when all data is taken from a single selected sub-distribution.

Characterizing a sub-distribution's min entropy always requires
some knowledge of how the non-determinism present in the system
manifests. Similarly, there must be some understanding of what
deterministic variation is expected.

To verify the first assumption,
there is no statistical test that can distinguish between complicated
deterministic behavior and non-deterministic behavior, so some design-based
argument is necessary. It is important to note that not all hardware
experiences this sort of variation, and indeed some types of hardware are
engineered so that their timing is both wholly deterministic and regular.

For the second assumption, characterizing a sub-distribution's min entropy
always requires some knowledge of how the non-determinism present in
the system manifests. Similarly, there must be some understanding of
what deterministic variation is expected.

We need to be able to argue that the entropy estimator producing the
non-IID track's result provides a reasonable min entropy lower bound
for the noise source and does not over-credit the min entropy due to
variation that is essentially deterministic in its origin.

In this analysis approach, once the noise source is configured to produce
a single identified sub-distribution, the SP 800-90B non-IID track can
then be used to generate the heuristic min entropy estimate.

The estimators vary in complexity and response to the data, so it isn't
clear how to demonstrate that the estimators provide a reasonable lower
bound for the source min entropy.


# Worked Examples
## Test System
### Identification of the Test System
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
> TSC for wall clock timer services... The features supported can be
> identified by using the CPUID instruction.

With Linux systems, the “invariant TSC” CPU feature is available when
both the `constant_tsc` and `nonstop_tsc` CPU feature flags are
present in /proc/cpuinfo.

### Relevant Test System Characteristics
On this platform, the source of timing information is the TSC, so the
counter is sufficiently fine-grained to support `JENT_MEMACCESSLOOP_EXP = 0`.
On a platform where the observed distribution is very narrow we
may have to increase this parameter. Note: increasing this parameter
too much will lead to nearly full entropy samples, which will result in
the system not fulfilling Property A. It is useful to use the smallest
value of `JENT_MEMACCESSLOOP_EXP` that yields a reasonable distribution.

The test system has a very complicated architecture, with many sources of
variation in execution time. Here we describe known sources of timing variation, which of these is being credited, and how the non-credited sources of timing variation are controlled in testing:
- Pipelining: The test code uses the `rdtscp` and `lfence` instructions to obtain the TSC value to reduce pipelining effects.
- Branch prediction: As `JENT_MEMORY_DEPTH_EXP` gets larger, the branch prediction will become more consistent.
- Frequency scaling: The hardware is effectively cooled and otherwise quiescent, so thermal throttling is kept to a minimum. Throttling for the purpose of power saving has been disabled through configuration.
- Context switching: The entire selected sub-distribution is much too low to include a context switch.
- Hardware interrupts: The entire selected sub-distribution is much too low to include results where the running core serviced a hardware interrupt.
- Executing on a different core: The test process was locked to a single core.
- Memory controller delays: This variation is included in the assessed distribution.
- Inter-core fabric delays: This variation is included in the assessed distribution.
- Memory cache hits vs cache misses at each cache level: The size of the memory region (and the way that the memory address is selected) forces the desired RAM I/O, and only the timing data from that corresponding sub-distribution contributes raw noise samples.
- RAM I/O delay: This variation is included in the assessed distribution.

## Selecting a Sub-Distribution
For Step #1, the `analyze_memsize.sh` script was used to generate
(non-decimated) data for settings between 10 and 30.

For Step #2, the following histograms were generated:

![Distributions Across Memory Sizes](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/distanim.gif)

Note: These histograms (and all following diagrams and specified values)
are with respect to the number of distinct increments of the counter. In
the tested architecture, the counter is incremented in multiples of 2,
so all values used by JEnt are divided by this common factor.

We see here that the memory read/update event mostly results in actual
RAM reads when `JENT_MEMORY_SIZE_EXP` is set to 27 or larger; the last three
histograms show essentially the same behavior; the terminal
distribution occurs when `JENT_MEMORY_SIZE_EXP` is 28 or higher.

For this evaluation, we proceed with the `JENT_MEMORY_SIZE_EXP` setting of 28
(resulting in a memory region of 256 MB). This results in the following histogram
for non-decimated data:

![IID Testing Results](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/non-dec-hist-28.svg)

In this diagram, we see a couple of spikes in the [50, 100] interval,
associated data being cached in the L3 cache. We are interested in
results that result in RAM I/O, so we are interested in the next two
(dominant) spikes in the interval [105, 185]. As such, for Step 3,
the distribution that we are interested in is in the interval [105, 185].

## The *Essentially IID* Analysis Approach Worked Example
### Assumptions
For **Assumption 1**: In the worked example, we essentially presume a version of Assumption
1 that applies only for the captured sub-distribution, that is: the
identified timing variation (which is largely determined by variations of
timings of RAM I/O) is non-deterministic; not all hardware experiences
this sort of variation, and indeed some types of hardware are engineered
so that their timing is both wholly deterministic and regular. Intel has
investigated this style of source, and found that RAM I/O variation has
a non-deterministic component.

For **Assumption 2**: To investigate the "essentially IID" property in Step 4, we used the
`analyze_depth.sh` script to generate 147 million samples for each
of the tested `JENT_MEMORY_DEPTH_EXP` settings, using the parameters
`JENT_MEMORY_SIZE_EXP = 28`, `JENT_DISTRIBUTION_MIN = 105`, and
`JENT_DISTRIBUTION_MAX = 185`. We then performed IID testing on each
of the 147 1-million sample subsets for each `JENT_MEMORY_DEPTH_EXP`
setting. The IID testing results were as follows:

![IID Testing Results](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/IID-testing.svg)

Here, a "round" is a full SP 800-90B IID test round conducted on one of
the disjoint data subsets of 1-million samples. A round is considered
to be passing if and only if all 22 of the IID tests pass for that
particular subset. The "Tests Passing" refers to the total proportion
of the IID tests that pass across all 147 tested subsets for each tested
memory depth setting.

This shows that the data seems to become increasingly close to IID
behavior as the `JENT_MEMORY_DEPTH_EXP` value is increased. Note that
this distribution family seems to satisfy all of the properties described
above:

* Property A: For `JENT_MEMORY_DEPTH_EXP = 0`, 100% of the 147 distinct
rounds fail the SP 800-90B Section 5 IID testing.
* Property B: The required number of each test pass when
`JENT_MEMORY_DEPTH_EXP = 11`.

We note that the data passes under the above interpretation for
`JENT_MEMORY_DEPTH_EXP = 11`.

### Results
Using the parameters `JENT_MEMORY_DEPTH_EXP = 11`, `JENT_MEMORY_SIZE_EXP =
28`, `JENT_DISTRIBUTION_MIN = 105`, and `JENT_DISTRIBUTION_MAX = 185`,
the source produces the following histogram.

![IID Testing Results](https://github.com/joshuaehill/jitterentropy-library/blob/MemOnly/tests/raw-entropy/final-hist.svg)

Now that the source is behaving as an IID source, we can directly produce
an estimate for the entropy, namely approximately

$$H_{\text{submitter}} = - \log_2 ( 0.0518297 ) \approx 4.27$$

bits of min entropy per symbol.

In the example above, the measurement shows that as much as 4.2 bits of
entropy can be credited which implies that the available amount of entropy
is more than what the Jitter RNG accounting requires, even when setting
`osr` to the smallest possible value (`osr = 1`).

## The *Single Sub-Distribution Empirical* Analysis Approach Worked Example
### Assumptions
For **Assumption 1**: In the worked example, we essentially presume
a version of Assumption 1 that applies only for the captured
sub-distribution, that is: the identified timing variation
(which is largely determined by variations of timings of RAM I/O) is
non-deterministic; Intel has investigated this
style of source run on a closely related architecture, and found that
RAM I/O variation has a non-deterministic component.

For **Assumption 2**: We rely on the complete set of SP
800-90B estimators (i.e., the non-IID track result) to provide a reasonable
lower bound for the min entropy produced by the noise source when all
data is taken from a single selected sub-distribution. In this analysis approach,
we have reduced
the data to include only the output of a single selected sub-distribution, with the
intent of removing any over crediting due to the contributions of other sub-distributions.

This assessment approach is considerably more conservative than in the
Original JEnt Analysis Approach, but it still relies on the SP 800-90B
min entropy estimators to produce a conservative lower bound.

### Results
Due to the sub-distribution selection (recall `JENT_DISTRIBUTION_MIN = 105`, and `JENT_DISTRIBUTION_MAX = 185`), the produced data can be directly represented as 8-bit values.
A sample of 147 million non-decimated samples (`JENT_MEMORY_DEPTH_EXP = 0`) were assessed using the non-IID track.
The results from the SP 800-90B non-IID statistical entropy estimation track
(`ea_noniid`, when using two verbose flags) is as follows:

```
[...]
H_bitstring = 0.48854235733379042
H_bitstring Per Symbol = 3.419796501336533
H_original = 3.8331319722313415
Assessed min entropy: 3.419796501336533
```

The last value gives you an **upper** bound for the min entropy per raw sample
that can be claimed within the SP 800-90B evaluation process.
In this analysis approach, we are operating under the assumption that the min entropy
reported by the SP 800-90B non-IID track is also a **lower** min entropy bound
(**Assumption 2** for this analysis approach), so
we can also conclude that

$$H_{\text{submitter}} \approx 3.4 \text{.}$$

In the example above, the measurement shows that as much as 3.4 bits of
entropy can be credited which implies that the available amount of entropy
is more than the Jitter RNG accounting requires even when setting
`osr` to the smallest possible value (`osr = 1`).

## Commentary

The decimation rate has a significant impact on the rate that the noise source
outputs data, and this has an impact on the rate that the library can
output conditioned data. On the test system with
`JENT_MEMORY_DEPTH_EXP = 0` (no decimation), this entropy source produces
approximately 287 conditioned outputs every second (where each output
is 256 bits). On average, the probabilistic decimation outputs a value
from the noise source approximately every

$$3 \times 2^{\text{JENT\\_MEMORY\\_DEPTH\\_EXP} - 1} - \frac{1}{2}$$

candidates, so the setting `JENT_MEMORY_DEPTH_EXP = 11` reduces the
output rate from the noise source by a factor of approximately 3071.5. As
it turns out, the conditioning time dwarfs the memory update time, so
this results in a rate of approximately 1.5 outputs per second (which is
considerably faster than if the noise source output rate was the determining
factor.)

With this degree of slowdown, is the use of the Essentially IID Analysis Approach
"worth it" over the Single Sub-Distribution Empirical Analysis Approach? First, it is important
to point out that even though `JENT_MEMORY_DEPTH_EXP` may look like it
is "throwing away" significant amounts of (possibly entropy-containing)
data, the decimated values are fed into the conditioner as "supplemental
data". As such, though no entropy can be formally claimed associated
with this data, in practice any entropy would be retained within the
conditioning function. As such, use of this value is expected to only
make the security of the system better, as considerably more data is
sent into the conditioning function.

The main analysis difference between the available analysis approaches can
be summarized as follows:
- The *Essentially IID Analysis Approach* deliberately operates the JEnt library using
parameters that allow for a straight-forward statistical argument for
a particular min entropy lower bound (but at the cost of a performance reduction).
- The *Single Sub-Distribution Empirical Analysis Approach* instead relies on the various
sub-distribution simplifications and the testing setup to provide a
distribution that is so simple that we expect that its min entropy can
be accurately assessed by the SP 800-90B min entropy estimators.
- The original *JEnt Analysis Approach* presumes that the a more
complicated distribution of the timing of both the memory
update (of a much smaller memory region that may predominantly resolve
to cache hits) and a large conditioning step can be empirically assessed
using the SP 800-90B min entropy estimators.

We note that performing the full non-IID testing on the decimated data
provides the following results:
```
[...]
H_bitstring = 0.50729119109721543
H_bitstring Per Symbol = 3.5510383376805081
H_original = 4.1991141990845975
Assessed min entropy: 3.5510383376805081
```
It is useful to note that, while the decimated data and non-decimated data
seem to produce similar SP 800-90B non-IID track results, IID testing
indicates that there is a significant difference between the decimated
and non-decimated data streams. This suggests that the non-decimated
data may suffer from some statistical flaws that may not have been
detected (and thus may not be fully accounted for) by the non-IID
entropy estimators. In this instance, the applied decimation helps prevent over
crediting entropy in the system.

# Authors
Stephan Mueller <smueller@chronox.de>
Joshua E. Hill <josh@keypair.us>
