#pragma D option quiet

txg-syncing
{
	this->dp = (dsl_pool_t *)arg0;
}

txg-syncing
/this->dp->dp_spa->spa_name == $$1/
{
	printf("%4dMB Dirty total\n", this->dp->dp_dirty_total / 1024 /1024);
}
