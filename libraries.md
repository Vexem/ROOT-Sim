# ROOT-Sim Libraries

To simplify the development of simulation models according to the speculative PDES paradigm, ROOT-Sim offers a set of libraries which can be used to implement important portions of simulation models, or to automatize tedious tasks.

In this section, we describe the available libraries, the exposed API, and we show some usage examples.

## Numerical Library

ROOT-Sim offers a fully-featured numerical library designed according to the Piece-Wise Determinism paradigm. The main idea behind this library is that if a Logical Process incurs into a Rollback, the seed which is associated with the random number generator associated with that LP must be rolled back as well. The numerical library provided by ROOT-Sim transparently does so, while if you rely on a different
numerical library, you must implement this feature by hand, if you want that a logical process is always given the same sequence of pseudo-random numbers, even when the execution is restarted from a previous simulation state.

The following functions are available in the ROOT-Sim numerical library. They can be used to draw samples from random distribution, which are commonly used in many simulation models.

### `Random()`

This function has the following signature:

`double Random(void);`

It returns a floating point number in between [0,1], according to a Uniform Distribution.

### `RandomRange()`

This function has the following signature:

`int RandomRange(int min, int max)`

It returns an integer number in between [`min`,`max`], according to a Uniform Distribution.

### `RandomRangeNonUniform()`

This function has the following signature:

`int RandomRangeNonUniform(int x, int min, int max)`

It returns an integer number in between [`min`,`max`]. The parameter `x` determines the incremented probability according to which a number is generated in the range, according to the following formula:

```c
(((RandomRange(0, x) | RandomRange(min, max))) % (max - min + 1)) + min
```

### `Expent()`

The signature of this function is:

`double Expent(double mean)`

It returns a floating point number according to an Exponential Distribution of mean value `mean`.

### `Normal()`

The signature of this function is:

`double Normal(void)`

It returns a floating point number according to a Normal Distribution with mean zero.

### `Gamma()`

The signature of this function is:

`double Gamma(int ia)`

It returns a floting point number according to a Gamma Distribution of Integer Order `ia`, i.e. a waiting time to the `ia`-th event in a Poisson process of unit mean.

### `Poisson()`

The signature of this function is:

`double Poisson(void)`

It returns the waiting time to the next event in a Poisson process of unit mean.

### `Zipf()`

The signature of this function is:

`int Zipf(double skew, int limit)`

It returns a random sample from a Zipf distribution.

## Agent-Based Modeling Library

## Topology Library

## JSON Parsing Library

