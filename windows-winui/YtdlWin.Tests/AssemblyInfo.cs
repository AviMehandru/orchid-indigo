/* Test parallelism is OFF for this assembly, and that is a correctness
 * requirement rather than a preference.
 *
 * Two pieces of state in the app under test are static by design:
 * Paths.EnvironmentOverride, which is the seam this suite uses to point the
 * app's notion of "home" at a fixture directory, and Health's dependency-probe
 * cache. xUnit runs test CLASSES in parallel by default, so with it on, one
 * class's redirected home is live while another class is reading the real
 * environment -- and the failure lands in whichever test happened to be
 * running, not in the one that caused it.
 *
 * The alternative is threading an environment through every call in Paths, and
 * that would be a worse app for the sake of a faster suite. This suite is
 * seconds long.
 */

using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]
