require 'debase'
require 'benchmark'

Debugger.start
if ARGV.empty?
    raise "ruby happy_path.rb number_of_active_breakpoints [step]"
end
max_breakpoints = ARGV[0].to_i
step = ARGV.length > 1 ? ARGV[1].to_i : 1
step =[step, 1].max

old_cache_misses = 0
["cache", "no_cache"].each do |mode|
    if mode == "no_cache"
        Debase::Breakpoint.disable_cache(true)
    end
    Benchmark.bm do |x|
        bps = 0
        while bps <= max_breakpoints do
            x.report("#{mode} #{bps}") {
                i = 0
                while i < 100000 do
                    i += step
                end
            }
            bps += 1
            # Line number explicitly overlaps with line in the hot loop
            Debugger.add_breakpoint("foo.rb", 23, nil)
        end
    end
    old_cache_misses = Debase::Breakpoint.cache_misses - old_cache_misses
    puts old_cache_misses
end



