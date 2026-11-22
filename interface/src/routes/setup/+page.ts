import type { PageLoad } from './$types';

export const load = (async () => {
	return {
		title: 'Machine Setup'
	};
}) satisfies PageLoad;
