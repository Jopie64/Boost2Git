// Copyright Dave Abrahams 2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef AST_DWA2013425_HPP
# define AST_DWA2013425_HPP

# include "path.hpp"
#include <boost/fusion/adapted/struct/define_struct.hpp>
#include <vector>
#include <string>
#include <set>

BOOST_FUSION_DEFINE_STRUCT((boost2git), ContentRule,
  (path, svn_path)
  (path, git_path)
  (int, line)
  )

BOOST_FUSION_DEFINE_STRUCT((boost2git), BranchRule,
  (std::size_t, min)
  (std::size_t, max)
  (path, svn_path)
  (std::string, git_branch_or_tag_name)
  (int, line)
  (char const*, git_ref_qualifier)
  )

BOOST_FUSION_DEFINE_STRUCT((boost2git), RepoRule,
  (bool, is_abstract)
  (int, line)
  (std::string, git_repo_name)
  (std::vector<std::string>, bases)
  (std::vector<std::string>, submodule_info)
  (std::size_t, minrev)
  (std::size_t, maxrev)
  (std::vector<boost2git::ContentRule>, content_rules)
  (std::vector<boost2git::BranchRule>, branch_rules)
  (std::vector<boost2git::BranchRule>, tag_rules)
  )

namespace boost2git
{
inline std::string git_ref_name(BranchRule const* b)
  {
  return b->git_ref_qualifier + b->git_branch_or_tag_name;
  }

struct RepoRuleByName
  {
  bool operator()(RepoRule const& lhs, RepoRule const& rhs) const
    {
    return lhs.git_repo_name < rhs.git_repo_name;
    }
  };

typedef std::multiset<RepoRule, RepoRuleByName> AST;
}

#endif // AST_DWA2013425_HPP
