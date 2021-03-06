/*
 * Hopscotch hash table debug
 */

#include "cond_print.h"

#include <cstddef> /* size_t */

/*
 * ===== table =====
 */

template <typename TableBase>
	auto impl::operator<<(
		std::ostream &o_
		, const table_print<TableBase> &t_
	) -> std::ostream &
	{
		auto &tbl = t_.get_table();
		for ( const auto &k : tbl )
		{
			o_ << cond_print(k.first, "(key)") << " -> "
				<< cond_print(k.second, "(mapped)") << "\n";
		}
		return o_;
	}

template <typename TableBase>
	auto impl::operator<<(
		std::ostream &o_
		, const table_dump<TableBase> &t_
	) -> std::ostream &
	{
		auto &tbl_base = t_.get_table();
		o_ << "Buckets\n";
		for ( std::size_t k = 0; k != tbl_base.bucket_count(); ++k )
		{
			auto sb = tbl_base.make_segment_and_bucket(k);
			bypass_lock<typename TableBase::bucket_t, const owner> owner_lk(tbl_base.locate_owner(sb), sb);
			bypass_lock<typename TableBase::bucket_t, const content<typename TableBase::value_type>>
				content_lk(
					tbl_base.locate_content(sb)
					, sb
				);

			if (
				owner_lk.ref().value(owner_lk) != 0
				||
				! content_lk.ref().is_clear()
			)
			{
				o_ << k << ": "
					<< make_bucket_print(tbl_base, owner_lk, content_lk)
					<< "\n";
			}
		}
		if (
			tbl_base.segment_count_actual() < tbl_base.segment_count_target()
		)
		{
			auto &loc = tbl_base._bc[tbl_base.segment_count()];
			if ( loc._buckets )
			{
				o_ << "Pending buckets\n";
				for ( std::size_t ks = 0; ks != tbl_base.bucket_count(); ++ks )
				{
					const auto kj = tbl_base.bucket_count() + ks;
					const auto sbj = tbl_base.make_segment_and_bucket(kj);
					bypass_lock<typename TableBase::bucket_t, const owner> owner_lk(loc._buckets[ks], sbj);
					bypass_lock<typename TableBase::bucket_t, const content<typename TableBase::value_type>>
						content_lk(
							loc._buckets[ks]
							, sbj
						);
					if (
						owner_lk.ref().value(owner_lk) != 0
						||
						! content_lk.ref().is_clear()
					)
					{
						o_ << kj << ": "
							<< make_bucket_print(tbl_base, owner_lk, content_lk)
							<< "\n";
					}
				}
			}
			else
			{
				o_ << "Resize in progress but no pending buckets\n";
			}
		}
		return o_;
	}
