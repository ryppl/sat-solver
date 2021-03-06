require File.expand_path(File.join(File.dirname(__FILE__), 'helper'))
#
# Load repository from rpm database
#
#   repo = pool.add_rpmdb( "/root/dir/of/system" )
# will create an unnamed repository by reading the rpm database
# and create a solvable for each installed package.
# Use
#   repo.name = "..."
# to name the repository
#
#

# test adding rpmdb
class RpmdbTest < Test::Unit::TestCase
  def test_rpmdb
    pool = Satsolver::Pool.new
    assert pool
    pool.arch = "i686"
    repo = pool.add_rpmdb( "/" )
    assert repo.size > 0
    puts "#{repo.size} installed packages"
    i = 1
    name = nil
    pool.each { |s|
      puts "#{s}: #{s.vendor}"
      name = s.name if i == 7
      i += 1
      break if i > 10
    }
    s = pool.find( name )
    puts "Seventh: #{s}"
  end
end
