require 'debase'
require 'benchmark'

Debugger.start
if ARGV.empty?
    raise "ruby happy_path.rb number_of_active_breakpoints [step]"
end
max_breakpoints = ARGV[0].to_i
step = ARGV.length > 1 ? ARGV[1].to_i : 1
step =[step, 1].max

Benchmark.bm do |x|
    bps = 0
    bps = []
    while bps <= max_breakpoints do
        x.report("#{bps}") {
            i = 0
            while i < 100000 do
                i += step
            end
        }
        bps += 1
        bps << Debugger.add_breakpoint("foo.rb", 500 + bps, nil)
    end
    bps.each do |bp|
        Debugger.remove_breakpoint(bp.id)
    end
    bps = []
end
